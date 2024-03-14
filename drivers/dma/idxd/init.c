// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <uapi/linux/idxd.h>
#include <linux/dmaengine.h>
#include "../dmaengine.h"
#include "registers.h"
#include "idxd.h"
#include "perfmon.h"

MODULE_VERSION(IDXD_DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
MODULE_IMPORT_NS(IDXD);

static bool sva = true;
module_param(sva, bool, 0644);
MODULE_PARM_DESC(sva, "Toggle SVA support on/off");

bool tc_override;
module_param(tc_override, bool, 0644);
MODULE_PARM_DESC(tc_override, "Override traffic class defaults");

#define DRV_NAME "idxd"

bool support_enqcmd;
DEFINE_IDA(idxd_ida);

static struct idxd_driver_data idxd_driver_data[] = {
	[IDXD_TYPE_DSA] = {
		.name_prefix = "dsa",
		.type = IDXD_TYPE_DSA,
		.compl_size = sizeof(struct dsa_completion_record),
		.align = 32,
		.dev_type = &dsa_device_type,
		.evl_cr_off = offsetof(struct dsa_evl_entry, cr),
		.cr_status_off = offsetof(struct dsa_completion_record, status),
		.cr_result_off = offsetof(struct dsa_completion_record, result),
	},
	[IDXD_TYPE_IAX] = {
		.name_prefix = "iax",
		.type = IDXD_TYPE_IAX,
		.compl_size = sizeof(struct iax_completion_record),
		.align = 64,
		.dev_type = &iax_device_type,
		.evl_cr_off = offsetof(struct iax_evl_entry, cr),
		.cr_status_off = offsetof(struct iax_completion_record, status),
		.cr_result_off = offsetof(struct iax_completion_record, error_code),
	},
};

static struct pci_device_id idxd_pci_tbl[] = {
	/* DSA ver 1.0 platforms */
	{ PCI_DEVICE_DATA(INTEL, DSA_SPR0, &idxd_driver_data[IDXD_TYPE_DSA]) },

	/* IAX ver 1.0 platforms */
	{ PCI_DEVICE_DATA(INTEL, IAX_SPR0, &idxd_driver_data[IDXD_TYPE_IAX]) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, idxd_pci_tbl);

static int idxd_setup_interrupts(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct idxd_irq_entry *ie;
	int i, msixcnt;
	int rc = 0;

	msixcnt = pci_msix_vec_count(pdev);
	if (msixcnt < 0) {
		dev_err(dev, "Not MSI-X interrupt capable.\n");
		return -ENOSPC;
	}
	idxd->irq_cnt = msixcnt;

	rc = pci_alloc_irq_vectors(pdev, msixcnt, msixcnt, PCI_IRQ_MSIX);
	if (rc != msixcnt) {
		dev_err(dev, "Failed enabling %d MSIX entries: %d\n", msixcnt, rc);
		return -ENOSPC;
	}
	dev_dbg(dev, "Enabled %d msix vectors\n", msixcnt);


	ie = idxd_get_ie(idxd, 0);
	ie->vector = pci_irq_vector(pdev, 0);
	rc = request_threaded_irq(ie->vector, NULL, idxd_misc_thread, 0, "idxd-misc", ie);
	if (rc < 0) {
		dev_err(dev, "Failed to allocate misc interrupt.\n");
		goto err_misc_irq;
	}
	dev_dbg(dev, "Requested idxd-misc handler on msix vector %d\n", ie->vector);

	for (i = 0; i < idxd->max_wqs; i++) {
		int msix_idx = i + 1;

		ie = idxd_get_ie(idxd, msix_idx);
		ie->id = msix_idx;
		ie->int_handle = INVALID_INT_HANDLE;
		ie->pasid = IOMMU_PASID_INVALID;

		spin_lock_init(&ie->list_lock);
		init_llist_head(&ie->pending_llist);
		INIT_LIST_HEAD(&ie->work_list);
	}

	idxd_unmask_error_interrupts(idxd);
	return 0;

 err_misc_irq:
	idxd_mask_error_interrupts(idxd);
	pci_free_irq_vectors(pdev);
	dev_err(dev, "No usable interrupts\n");
	return rc;
}

static void idxd_cleanup_interrupts(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct idxd_irq_entry *ie;
	int msixcnt;

	msixcnt = pci_msix_vec_count(pdev);
	if (msixcnt <= 0)
		return;

	ie = idxd_get_ie(idxd, 0);
	idxd_mask_error_interrupts(idxd);
	free_irq(ie->vector, ie);
	pci_free_irq_vectors(pdev);
}

static int idxd_setup_wqs(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	struct idxd_wq *wq;
	struct device *conf_dev;
	int i, rc;

	idxd->wqs = kcalloc_node(idxd->max_wqs, sizeof(struct idxd_wq *),
				 GFP_KERNEL, dev_to_node(dev));
	if (!idxd->wqs)
		return -ENOMEM;

	idxd->wq_enable_map = bitmap_zalloc_node(idxd->max_wqs, GFP_KERNEL, dev_to_node(dev));
	if (!idxd->wq_enable_map) {
		kfree(idxd->wqs);
		return -ENOMEM;
	}

	for (i = 0; i < idxd->max_wqs; i++) {
		wq = kzalloc_node(sizeof(*wq), GFP_KERNEL, dev_to_node(dev));
		if (!wq) {
			rc = -ENOMEM;
			goto err;
		}

		idxd_dev_set_type(&wq->idxd_dev, IDXD_DEV_WQ);
		conf_dev = wq_confdev(wq);
		wq->id = i;
		wq->idxd = idxd;
		device_initialize(wq_confdev(wq));
		conf_dev->parent = idxd_confdev(idxd);
		conf_dev->bus = &dsa_bus_type;
		conf_dev->type = &idxd_wq_device_type;
		rc = dev_set_name(conf_dev, "wq%d.%d", idxd->id, wq->id);
		if (rc < 0) {
			put_device(conf_dev);
			goto err;
		}

		mutex_init(&wq->wq_lock);
		init_waitqueue_head(&wq->err_queue);
		init_completion(&wq->wq_dead);
		init_completion(&wq->wq_resurrect);
		wq->max_xfer_bytes = WQ_DEFAULT_MAX_XFER;
		idxd_wq_set_max_batch_size(idxd->data->type, wq, WQ_DEFAULT_MAX_BATCH);
		wq->enqcmds_retries = IDXD_ENQCMDS_RETRIES;
		wq->wqcfg = kzalloc_node(idxd->wqcfg_size, GFP_KERNEL, dev_to_node(dev));
		if (!wq->wqcfg) {
			put_device(conf_dev);
			rc = -ENOMEM;
			goto err;
		}

		if (idxd->hw.wq_cap.op_config) {
			wq->opcap_bmap = bitmap_zalloc(IDXD_MAX_OPCAP_BITS, GFP_KERNEL);
			if (!wq->opcap_bmap) {
				put_device(conf_dev);
				rc = -ENOMEM;
				goto err;
			}
			bitmap_copy(wq->opcap_bmap, idxd->opcap_bmap, IDXD_MAX_OPCAP_BITS);
		}
		mutex_init(&wq->uc_lock);
		xa_init(&wq->upasid_xa);
		idxd->wqs[i] = wq;
	}

	return 0;

 err:
	while (--i >= 0) {
		wq = idxd->wqs[i];
		conf_dev = wq_confdev(wq);
		put_device(conf_dev);
	}
	return rc;
}

static int idxd_setup_engines(struct idxd_device *idxd)
{
	struct idxd_engine *engine;
	struct device *dev = &idxd->pdev->dev;
	struct device *conf_dev;
	int i, rc;

	idxd->engines = kcalloc_node(idxd->max_engines, sizeof(struct idxd_engine *),
				     GFP_KERNEL, dev_to_node(dev));
	if (!idxd->engines)
		return -ENOMEM;

	for (i = 0; i < idxd->max_engines; i++) {
		engine = kzalloc_node(sizeof(*engine), GFP_KERNEL, dev_to_node(dev));
		if (!engine) {
			rc = -ENOMEM;
			goto err;
		}

		idxd_dev_set_type(&engine->idxd_dev, IDXD_DEV_ENGINE);
		conf_dev = engine_confdev(engine);
		engine->id = i;
		engine->idxd = idxd;
		device_initialize(conf_dev);
		conf_dev->parent = idxd_confdev(idxd);
		conf_dev->bus = &dsa_bus_type;
		conf_dev->type = &idxd_engine_device_type;
		rc = dev_set_name(conf_dev, "engine%d.%d", idxd->id, engine->id);
		if (rc < 0) {
			put_device(conf_dev);
			goto err;
		}

		idxd->engines[i] = engine;
	}

	return 0;

 err:
	while (--i >= 0) {
		engine = idxd->engines[i];
		conf_dev = engine_confdev(engine);
		put_device(conf_dev);
	}
	return rc;
}

static int idxd_setup_groups(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	struct device *conf_dev;
	struct idxd_group *group;
	int i, rc;

	idxd->groups = kcalloc_node(idxd->max_groups, sizeof(struct idxd_group *),
				    GFP_KERNEL, dev_to_node(dev));
	if (!idxd->groups)
		return -ENOMEM;

	for (i = 0; i < idxd->max_groups; i++) {
		group = kzalloc_node(sizeof(*group), GFP_KERNEL, dev_to_node(dev));
		if (!group) {
			rc = -ENOMEM;
			goto err;
		}

		idxd_dev_set_type(&group->idxd_dev, IDXD_DEV_GROUP);
		conf_dev = group_confdev(group);
		group->id = i;
		group->idxd = idxd;
		device_initialize(conf_dev);
		conf_dev->parent = idxd_confdev(idxd);
		conf_dev->bus = &dsa_bus_type;
		conf_dev->type = &idxd_group_device_type;
		rc = dev_set_name(conf_dev, "group%d.%d", idxd->id, group->id);
		if (rc < 0) {
			put_device(conf_dev);
			goto err;
		}

		idxd->groups[i] = group;
		if (idxd->hw.version <= DEVICE_VERSION_2 && !tc_override) {
			group->tc_a = 1;
			group->tc_b = 1;
		} else {
			group->tc_a = -1;
			group->tc_b = -1;
		}
		/*
		 * The default value is the same as the value of
		 * total read buffers in GRPCAP.
		 */
		group->rdbufs_allowed = idxd->max_rdbufs;
	}

	return 0;

 err:
	while (--i >= 0) {
		group = idxd->groups[i];
		put_device(group_confdev(group));
	}
	return rc;
}

static void idxd_cleanup_internals(struct idxd_device *idxd)
{
	int i;

	for (i = 0; i < idxd->max_groups; i++)
		put_device(group_confdev(idxd->groups[i]));
	for (i = 0; i < idxd->max_engines; i++)
		put_device(engine_confdev(idxd->engines[i]));
	for (i = 0; i < idxd->max_wqs; i++)
		put_device(wq_confdev(idxd->wqs[i]));
	destroy_workqueue(idxd->wq);
}

static int idxd_init_evl(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	struct idxd_evl *evl;

	if (idxd->hw.gen_cap.evl_support == 0)
		return 0;

	evl = kzalloc_node(sizeof(*evl), GFP_KERNEL, dev_to_node(dev));
	if (!evl)
		return -ENOMEM;

	spin_lock_init(&evl->lock);
	evl->size = IDXD_EVL_SIZE_MIN;

	idxd->evl_cache = kmem_cache_create(dev_name(idxd_confdev(idxd)),
					    sizeof(struct idxd_evl_fault) + evl_ent_size(idxd),
					    0, 0, NULL);
	if (!idxd->evl_cache) {
		kfree(evl);
		return -ENOMEM;
	}

	idxd->evl = evl;
	return 0;
}

static int idxd_setup_internals(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	int rc, i;

	init_waitqueue_head(&idxd->cmd_waitq);

	rc = idxd_setup_wqs(idxd);
	if (rc < 0)
		goto err_wqs;

	rc = idxd_setup_engines(idxd);
	if (rc < 0)
		goto err_engine;

	rc = idxd_setup_groups(idxd);
	if (rc < 0)
		goto err_group;

	idxd->wq = create_workqueue(dev_name(dev));
	if (!idxd->wq) {
		rc = -ENOMEM;
		goto err_wkq_create;
	}

	rc = idxd_init_evl(idxd);
	if (rc < 0)
		goto err_evl;

	return 0;

 err_evl:
	destroy_workqueue(idxd->wq);
 err_wkq_create:
	for (i = 0; i < idxd->max_groups; i++)
		put_device(group_confdev(idxd->groups[i]));
 err_group:
	for (i = 0; i < idxd->max_engines; i++)
		put_device(engine_confdev(idxd->engines[i]));
 err_engine:
	for (i = 0; i < idxd->max_wqs; i++)
		put_device(wq_confdev(idxd->wqs[i]));
 err_wqs:
	return rc;
}

static void idxd_read_table_offsets(struct idxd_device *idxd)
{
	union offsets_reg offsets;
	struct device *dev = &idxd->pdev->dev;

	offsets.bits[0] = ioread64(idxd->reg_base + IDXD_TABLE_OFFSET);
	offsets.bits[1] = ioread64(idxd->reg_base + IDXD_TABLE_OFFSET + sizeof(u64));
	idxd->grpcfg_offset = offsets.grpcfg * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Group Config Offset: %#x\n", idxd->grpcfg_offset);
	idxd->wqcfg_offset = offsets.wqcfg * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Work Queue Config Offset: %#x\n", idxd->wqcfg_offset);
	idxd->msix_perm_offset = offsets.msix_perm * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD MSIX Permission Offset: %#x\n", idxd->msix_perm_offset);
	idxd->perfmon_offset = offsets.perfmon * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Perfmon Offset: %#x\n", idxd->perfmon_offset);
}

void multi_u64_to_bmap(unsigned long *bmap, u64 *val, int count)
{
	int i, j, nr;

	for (i = 0, nr = 0; i < count; i++) {
		for (j = 0; j < BITS_PER_LONG_LONG; j++) {
			if (val[i] & BIT(j))
				set_bit(nr, bmap);
			nr++;
		}
	}
}

static void idxd_read_caps(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	int i;

	/* reading generic capabilities */
	idxd->hw.gen_cap.bits = ioread64(idxd->reg_base + IDXD_GENCAP_OFFSET);
	dev_dbg(dev, "gen_cap: %#llx\n", idxd->hw.gen_cap.bits);

	if (idxd->hw.gen_cap.cmd_cap) {
		idxd->hw.cmd_cap = ioread32(idxd->reg_base + IDXD_CMDCAP_OFFSET);
		dev_dbg(dev, "cmd_cap: %#x\n", idxd->hw.cmd_cap);
	}

	/* reading command capabilities */
	if (idxd->hw.cmd_cap & BIT(IDXD_CMD_REQUEST_INT_HANDLE))
		idxd->request_int_handles = true;

	idxd->max_xfer_bytes = 1ULL << idxd->hw.gen_cap.max_xfer_shift;
	dev_dbg(dev, "max xfer size: %llu bytes\n", idxd->max_xfer_bytes);
	idxd_set_max_batch_size(idxd->data->type, idxd, 1U << idxd->hw.gen_cap.max_batch_shift);
	dev_dbg(dev, "max batch size: %u\n", idxd->max_batch_size);
	if (idxd->hw.gen_cap.config_en)
		set_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags);

	/* reading group capabilities */
	idxd->hw.group_cap.bits =
		ioread64(idxd->reg_base + IDXD_GRPCAP_OFFSET);
	dev_dbg(dev, "group_cap: %#llx\n", idxd->hw.group_cap.bits);
	idxd->max_groups = idxd->hw.group_cap.num_groups;
	dev_dbg(dev, "max groups: %u\n", idxd->max_groups);
	idxd->max_rdbufs = idxd->hw.group_cap.total_rdbufs;
	dev_dbg(dev, "max read buffers: %u\n", idxd->max_rdbufs);
	idxd->nr_rdbufs = idxd->max_rdbufs;

	/* read engine capabilities */
	idxd->hw.engine_cap.bits =
		ioread64(idxd->reg_base + IDXD_ENGCAP_OFFSET);
	dev_dbg(dev, "engine_cap: %#llx\n", idxd->hw.engine_cap.bits);
	idxd->max_engines = idxd->hw.engine_cap.num_engines;
	dev_dbg(dev, "max engines: %u\n", idxd->max_engines);

	/* read workqueue capabilities */
	idxd->hw.wq_cap.bits = ioread64(idxd->reg_base + IDXD_WQCAP_OFFSET);
	dev_dbg(dev, "wq_cap: %#llx\n", idxd->hw.wq_cap.bits);
	idxd->max_wq_size = idxd->hw.wq_cap.total_wq_size;
	dev_dbg(dev, "total workqueue size: %u\n", idxd->max_wq_size);
	idxd->max_wqs = idxd->hw.wq_cap.num_wqs;
	dev_dbg(dev, "max workqueues: %u\n", idxd->max_wqs);
	idxd->wqcfg_size = 1 << (idxd->hw.wq_cap.wqcfg_size + IDXD_WQCFG_MIN);
	dev_dbg(dev, "wqcfg size: %u\n", idxd->wqcfg_size);

	/* reading operation capabilities */
	for (i = 0; i < 4; i++) {
		idxd->hw.opcap.bits[i] = ioread64(idxd->reg_base +
				IDXD_OPCAP_OFFSET + i * sizeof(u64));
		dev_dbg(dev, "opcap[%d]: %#llx\n", i, idxd->hw.opcap.bits[i]);
	}
	multi_u64_to_bmap(idxd->opcap_bmap, &idxd->hw.opcap.bits[0], 4);

	/* read iaa cap */
	if (idxd->data->type == IDXD_TYPE_IAX && idxd->hw.version >= DEVICE_VERSION_2)
		idxd->hw.iaa_cap.bits = ioread64(idxd->reg_base + IDXD_IAACAP_OFFSET);
}

static struct idxd_device *idxd_alloc(struct pci_dev *pdev, struct idxd_driver_data *data)
{
	struct device *dev = &pdev->dev;
	struct device *conf_dev;
	struct idxd_device *idxd;
	int rc;

	idxd = kzalloc_node(sizeof(*idxd), GFP_KERNEL, dev_to_node(dev));
	if (!idxd)
		return NULL;

	conf_dev = idxd_confdev(idxd);
	idxd->pdev = pdev;
	idxd->data = data;
	idxd_dev_set_type(&idxd->idxd_dev, idxd->data->type);
	idxd->id = ida_alloc(&idxd_ida, GFP_KERNEL);
	if (idxd->id < 0)
		return NULL;

	idxd->opcap_bmap = bitmap_zalloc_node(IDXD_MAX_OPCAP_BITS, GFP_KERNEL, dev_to_node(dev));
	if (!idxd->opcap_bmap) {
		ida_free(&idxd_ida, idxd->id);
		return NULL;
	}

	device_initialize(conf_dev);
	conf_dev->parent = dev;
	conf_dev->bus = &dsa_bus_type;
	conf_dev->type = idxd->data->dev_type;
	rc = dev_set_name(conf_dev, "%s%d", idxd->data->name_prefix, idxd->id);
	if (rc < 0) {
		put_device(conf_dev);
		return NULL;
	}

	spin_lock_init(&idxd->dev_lock);
	spin_lock_init(&idxd->cmd_lock);

	return idxd;
}

static int idxd_enable_system_pasid(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct iommu_domain *domain;
	ioasid_t pasid;
	int ret;

	/*
	 * Attach a global PASID to the DMA domain so that we can use ENQCMDS
	 * to submit work on buffers mapped by DMA API.
	 */
	domain = iommu_get_domain_for_dev(dev);
	if (!domain)
		return -EPERM;

	pasid = iommu_alloc_global_pasid(dev);
	if (pasid == IOMMU_PASID_INVALID)
		return -ENOSPC;

	/*
	 * DMA domain is owned by the driver, it should support all valid
	 * types such as DMA-FQ, identity, etc.
	 */
	ret = iommu_attach_device_pasid(domain, dev, pasid);
	if (ret) {
		dev_err(dev, "failed to attach device pasid %d, domain type %d",
			pasid, domain->type);
		iommu_free_global_pasid(pasid);
		return ret;
	}

	/* Since we set user privilege for kernel DMA, enable completion IRQ */
	idxd_set_user_intr(idxd, 1);
	idxd->pasid = pasid;

	return ret;
}

static void idxd_disable_system_pasid(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct iommu_domain *domain;

	domain = iommu_get_domain_for_dev(dev);
	if (!domain)
		return;

	iommu_detach_device_pasid(domain, dev, idxd->pasid);
	iommu_free_global_pasid(idxd->pasid);

	idxd_set_user_intr(idxd, 0);
	idxd->sva = NULL;
	idxd->pasid = IOMMU_PASID_INVALID;
}

static int idxd_enable_sva(struct pci_dev *pdev)
{
	int ret;

	ret = iommu_dev_enable_feature(&pdev->dev, IOMMU_DEV_FEAT_IOPF);
	if (ret)
		return ret;

	ret = iommu_dev_enable_feature(&pdev->dev, IOMMU_DEV_FEAT_SVA);
	if (ret)
		iommu_dev_disable_feature(&pdev->dev, IOMMU_DEV_FEAT_IOPF);

	return ret;
}

static void idxd_disable_sva(struct pci_dev *pdev)
{
	iommu_dev_disable_feature(&pdev->dev, IOMMU_DEV_FEAT_SVA);
	iommu_dev_disable_feature(&pdev->dev, IOMMU_DEV_FEAT_IOPF);
}

static int idxd_probe(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	int rc;

	dev_dbg(dev, "%s entered and resetting device\n", __func__);
	rc = idxd_device_init_reset(idxd);
	if (rc < 0)
		return rc;

	dev_dbg(dev, "IDXD reset complete\n");

	if (IS_ENABLED(CONFIG_INTEL_IDXD_SVM) && sva) {
		if (idxd_enable_sva(pdev)) {
			dev_warn(dev, "Unable to turn on user SVA feature.\n");
		} else {
			set_bit(IDXD_FLAG_USER_PASID_ENABLED, &idxd->flags);

			rc = idxd_enable_system_pasid(idxd);
			if (rc)
				dev_warn(dev, "No in-kernel DMA with PASID. %d\n", rc);
			else
				set_bit(IDXD_FLAG_PASID_ENABLED, &idxd->flags);
		}
	} else if (!sva) {
		dev_warn(dev, "User forced SVA off via module param.\n");
	}

	idxd_read_caps(idxd);
	idxd_read_table_offsets(idxd);

	rc = idxd_setup_internals(idxd);
	if (rc)
		goto err;

	/* If the configs are readonly, then load them from device */
	if (!test_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags)) {
		dev_dbg(dev, "Loading RO device config\n");
		rc = idxd_device_load_config(idxd);
		if (rc < 0)
			goto err_config;
	}

	rc = idxd_setup_interrupts(idxd);
	if (rc)
		goto err_config;

	idxd->major = idxd_cdev_get_major(idxd);

	rc = perfmon_pmu_init(idxd);
	if (rc < 0)
		dev_warn(dev, "Failed to initialize perfmon. No PMU support: %d\n", rc);

	dev_dbg(dev, "IDXD device %d probed successfully\n", idxd->id);
	return 0;

 err_config:
	idxd_cleanup_internals(idxd);
 err:
	if (device_pasid_enabled(idxd))
		idxd_disable_system_pasid(idxd);
	if (device_user_pasid_enabled(idxd))
		idxd_disable_sva(pdev);
	return rc;
}

static void idxd_cleanup(struct idxd_device *idxd)
{
	perfmon_pmu_remove(idxd);
	idxd_cleanup_interrupts(idxd);
	idxd_cleanup_internals(idxd);
	if (device_pasid_enabled(idxd))
		idxd_disable_system_pasid(idxd);
	if (device_user_pasid_enabled(idxd))
		idxd_disable_sva(idxd->pdev);
}

static int idxd_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct idxd_device *idxd;
	struct idxd_driver_data *data = (struct idxd_driver_data *)id->driver_data;
	int rc;

	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	dev_dbg(dev, "Alloc IDXD context\n");
	idxd = idxd_alloc(pdev, data);
	if (!idxd) {
		rc = -ENOMEM;
		goto err_idxd_alloc;
	}

	dev_dbg(dev, "Mapping BARs\n");
	idxd->reg_base = pci_iomap(pdev, IDXD_MMIO_BAR, 0);
	if (!idxd->reg_base) {
		rc = -ENOMEM;
		goto err_iomap;
	}

	dev_dbg(dev, "Set DMA masks\n");
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (rc)
		goto err;

	dev_dbg(dev, "Set PCI master\n");
	pci_set_master(pdev);
	pci_set_drvdata(pdev, idxd);

	idxd->hw.version = ioread32(idxd->reg_base + IDXD_VER_OFFSET);
	rc = idxd_probe(idxd);
	if (rc) {
		dev_err(dev, "Intel(R) IDXD DMA Engine init failed\n");
		goto err;
	}

	rc = idxd_register_devices(idxd);
	if (rc) {
		dev_err(dev, "IDXD sysfs setup failed\n");
		goto err_dev_register;
	}

	rc = idxd_device_init_debugfs(idxd);
	if (rc)
		dev_warn(dev, "IDXD debugfs failed to setup\n");

	dev_info(&pdev->dev, "Intel(R) Accelerator Device (v%x)\n",
		 idxd->hw.version);

	return 0;

 err_dev_register:
	idxd_cleanup(idxd);
 err:
	pci_iounmap(pdev, idxd->reg_base);
 err_iomap:
	put_device(idxd_confdev(idxd));
 err_idxd_alloc:
	pci_disable_device(pdev);
	return rc;
}

void idxd_wqs_quiesce(struct idxd_device *idxd)
{
	struct idxd_wq *wq;
	int i;

	for (i = 0; i < idxd->max_wqs; i++) {
		wq = idxd->wqs[i];
		if (wq->state == IDXD_WQ_ENABLED && wq->type == IDXD_WQT_KERNEL)
			idxd_wq_quiesce(wq);
	}
}

static void idxd_shutdown(struct pci_dev *pdev)
{
	struct idxd_device *idxd = pci_get_drvdata(pdev);
	struct idxd_irq_entry *irq_entry;
	int rc;

	rc = idxd_device_disable(idxd);
	if (rc)
		dev_err(&pdev->dev, "Disabling device failed\n");

	irq_entry = &idxd->ie;
	synchronize_irq(irq_entry->vector);
	idxd_mask_error_interrupts(idxd);
	flush_workqueue(idxd->wq);
}

static void idxd_remove(struct pci_dev *pdev)
{
	struct idxd_device *idxd = pci_get_drvdata(pdev);
	struct idxd_irq_entry *irq_entry;

	idxd_unregister_devices(idxd);
	/*
	 * When ->release() is called for the idxd->conf_dev, it frees all the memory related
	 * to the idxd context. The driver still needs those bits in order to do the rest of
	 * the cleanup. However, we do need to unbound the idxd sub-driver. So take a ref
	 * on the device here to hold off the freeing while allowing the idxd sub-driver
	 * to unbind.
	 */
	get_device(idxd_confdev(idxd));
	device_unregister(idxd_confdev(idxd));
	idxd_shutdown(pdev);
	if (device_pasid_enabled(idxd))
		idxd_disable_system_pasid(idxd);
	idxd_device_remove_debugfs(idxd);

	irq_entry = idxd_get_ie(idxd, 0);
	free_irq(irq_entry->vector, irq_entry);
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, idxd->reg_base);
	if (device_user_pasid_enabled(idxd))
		idxd_disable_sva(pdev);
	pci_disable_device(pdev);
	destroy_workqueue(idxd->wq);
	perfmon_pmu_remove(idxd);
	put_device(idxd_confdev(idxd));
}

static struct pci_driver idxd_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= idxd_pci_tbl,
	.probe		= idxd_pci_probe,
	.remove		= idxd_remove,
	.shutdown	= idxd_shutdown,
};

static int __init idxd_init_module(void)
{
	int err;

	/*
	 * If the CPU does not support MOVDIR64B or ENQCMDS, there's no point in
	 * enumerating the device. We can not utilize it.
	 */
	if (!cpu_feature_enabled(X86_FEATURE_MOVDIR64B)) {
		pr_warn("idxd driver failed to load without MOVDIR64B.\n");
		return -ENODEV;
	}

	if (!cpu_feature_enabled(X86_FEATURE_ENQCMD))
		pr_warn("Platform does not have ENQCMD(S) support.\n");
	else
		support_enqcmd = true;

	perfmon_init();

	err = idxd_driver_register(&idxd_drv);
	if (err < 0)
		goto err_idxd_driver_register;

	err = idxd_driver_register(&idxd_dmaengine_drv);
	if (err < 0)
		goto err_idxd_dmaengine_driver_register;

	err = idxd_driver_register(&idxd_user_drv);
	if (err < 0)
		goto err_idxd_user_driver_register;

	err = idxd_cdev_register();
	if (err)
		goto err_cdev_register;

	err = idxd_init_debugfs();
	if (err)
		goto err_debugfs;

	err = pci_register_driver(&idxd_pci_driver);
	if (err)
		goto err_pci_register;

	return 0;

err_pci_register:
	idxd_remove_debugfs();
err_debugfs:
	idxd_cdev_remove();
err_cdev_register:
	idxd_driver_unregister(&idxd_user_drv);
err_idxd_user_driver_register:
	idxd_driver_unregister(&idxd_dmaengine_drv);
err_idxd_dmaengine_driver_register:
	idxd_driver_unregister(&idxd_drv);
err_idxd_driver_register:
	return err;
}
module_init(idxd_init_module);

static void __exit idxd_exit_module(void)
{
	idxd_driver_unregister(&idxd_user_drv);
	idxd_driver_unregister(&idxd_dmaengine_drv);
	idxd_driver_unregister(&idxd_drv);
	pci_unregister_driver(&idxd_pci_driver);
	idxd_cdev_remove();
	perfmon_exit();
	idxd_remove_debugfs();
}
module_exit(idxd_exit_module);
