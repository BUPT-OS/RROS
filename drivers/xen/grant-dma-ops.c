// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xen grant DMA-mapping layer - contains special DMA-mapping routines
 * for providing grant references as DMA addresses to be used by frontends
 * (e.g. virtio) in Xen guests
 *
 * Copyright (c) 2021, Juergen Gross <jgross@suse.com>
 */

#include <linux/module.h>
#include <linux/dma-map-ops.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/pfn.h>
#include <linux/xarray.h>
#include <linux/virtio_anchor.h>
#include <linux/virtio.h>
#include <xen/xen.h>
#include <xen/xen-ops.h>
#include <xen/grant_table.h>

struct xen_grant_dma_data {
	/* The ID of backend domain */
	domid_t backend_domid;
	/* Is device behaving sane? */
	bool broken;
};

static DEFINE_XARRAY_FLAGS(xen_grant_dma_devices, XA_FLAGS_LOCK_IRQ);

#define XEN_GRANT_DMA_ADDR_OFF	(1ULL << 63)

static inline dma_addr_t grant_to_dma(grant_ref_t grant)
{
	return XEN_GRANT_DMA_ADDR_OFF | ((dma_addr_t)grant << XEN_PAGE_SHIFT);
}

static inline grant_ref_t dma_to_grant(dma_addr_t dma)
{
	return (grant_ref_t)((dma & ~XEN_GRANT_DMA_ADDR_OFF) >> XEN_PAGE_SHIFT);
}

static struct xen_grant_dma_data *find_xen_grant_dma_data(struct device *dev)
{
	struct xen_grant_dma_data *data;
	unsigned long flags;

	xa_lock_irqsave(&xen_grant_dma_devices, flags);
	data = xa_load(&xen_grant_dma_devices, (unsigned long)dev);
	xa_unlock_irqrestore(&xen_grant_dma_devices, flags);

	return data;
}

static int store_xen_grant_dma_data(struct device *dev,
				    struct xen_grant_dma_data *data)
{
	unsigned long flags;
	int ret;

	xa_lock_irqsave(&xen_grant_dma_devices, flags);
	ret = xa_err(__xa_store(&xen_grant_dma_devices, (unsigned long)dev, data,
			GFP_ATOMIC));
	xa_unlock_irqrestore(&xen_grant_dma_devices, flags);

	return ret;
}

/*
 * DMA ops for Xen frontends (e.g. virtio).
 *
 * Used to act as a kind of software IOMMU for Xen guests by using grants as
 * DMA addresses.
 * Such a DMA address is formed by using the grant reference as a frame
 * number and setting the highest address bit (this bit is for the backend
 * to be able to distinguish it from e.g. a mmio address).
 */
static void *xen_grant_dma_alloc(struct device *dev, size_t size,
				 dma_addr_t *dma_handle, gfp_t gfp,
				 unsigned long attrs)
{
	struct xen_grant_dma_data *data;
	unsigned int i, n_pages = XEN_PFN_UP(size);
	unsigned long pfn;
	grant_ref_t grant;
	void *ret;

	data = find_xen_grant_dma_data(dev);
	if (!data)
		return NULL;

	if (unlikely(data->broken))
		return NULL;

	ret = alloc_pages_exact(n_pages * XEN_PAGE_SIZE, gfp);
	if (!ret)
		return NULL;

	pfn = virt_to_pfn(ret);

	if (gnttab_alloc_grant_reference_seq(n_pages, &grant)) {
		free_pages_exact(ret, n_pages * XEN_PAGE_SIZE);
		return NULL;
	}

	for (i = 0; i < n_pages; i++) {
		gnttab_grant_foreign_access_ref(grant + i, data->backend_domid,
				pfn_to_gfn(pfn + i), 0);
	}

	*dma_handle = grant_to_dma(grant);

	return ret;
}

static void xen_grant_dma_free(struct device *dev, size_t size, void *vaddr,
			       dma_addr_t dma_handle, unsigned long attrs)
{
	struct xen_grant_dma_data *data;
	unsigned int i, n_pages = XEN_PFN_UP(size);
	grant_ref_t grant;

	data = find_xen_grant_dma_data(dev);
	if (!data)
		return;

	if (unlikely(data->broken))
		return;

	grant = dma_to_grant(dma_handle);

	for (i = 0; i < n_pages; i++) {
		if (unlikely(!gnttab_end_foreign_access_ref(grant + i))) {
			dev_alert(dev, "Grant still in use by backend domain, disabled for further use\n");
			data->broken = true;
			return;
		}
	}

	gnttab_free_grant_reference_seq(grant, n_pages);

	free_pages_exact(vaddr, n_pages * XEN_PAGE_SIZE);
}

static struct page *xen_grant_dma_alloc_pages(struct device *dev, size_t size,
					      dma_addr_t *dma_handle,
					      enum dma_data_direction dir,
					      gfp_t gfp)
{
	void *vaddr;

	vaddr = xen_grant_dma_alloc(dev, size, dma_handle, gfp, 0);
	if (!vaddr)
		return NULL;

	return virt_to_page(vaddr);
}

static void xen_grant_dma_free_pages(struct device *dev, size_t size,
				     struct page *vaddr, dma_addr_t dma_handle,
				     enum dma_data_direction dir)
{
	xen_grant_dma_free(dev, size, page_to_virt(vaddr), dma_handle, 0);
}

static dma_addr_t xen_grant_dma_map_page(struct device *dev, struct page *page,
					 unsigned long offset, size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	struct xen_grant_dma_data *data;
	unsigned long dma_offset = xen_offset_in_page(offset),
			pfn_offset = XEN_PFN_DOWN(offset);
	unsigned int i, n_pages = XEN_PFN_UP(dma_offset + size);
	grant_ref_t grant;
	dma_addr_t dma_handle;

	if (WARN_ON(dir == DMA_NONE))
		return DMA_MAPPING_ERROR;

	data = find_xen_grant_dma_data(dev);
	if (!data)
		return DMA_MAPPING_ERROR;

	if (unlikely(data->broken))
		return DMA_MAPPING_ERROR;

	if (gnttab_alloc_grant_reference_seq(n_pages, &grant))
		return DMA_MAPPING_ERROR;

	for (i = 0; i < n_pages; i++) {
		gnttab_grant_foreign_access_ref(grant + i, data->backend_domid,
				pfn_to_gfn(page_to_xen_pfn(page) + i + pfn_offset),
				dir == DMA_TO_DEVICE);
	}

	dma_handle = grant_to_dma(grant) + dma_offset;

	return dma_handle;
}

static void xen_grant_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
				     size_t size, enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct xen_grant_dma_data *data;
	unsigned long dma_offset = xen_offset_in_page(dma_handle);
	unsigned int i, n_pages = XEN_PFN_UP(dma_offset + size);
	grant_ref_t grant;

	if (WARN_ON(dir == DMA_NONE))
		return;

	data = find_xen_grant_dma_data(dev);
	if (!data)
		return;

	if (unlikely(data->broken))
		return;

	grant = dma_to_grant(dma_handle);

	for (i = 0; i < n_pages; i++) {
		if (unlikely(!gnttab_end_foreign_access_ref(grant + i))) {
			dev_alert(dev, "Grant still in use by backend domain, disabled for further use\n");
			data->broken = true;
			return;
		}
	}

	gnttab_free_grant_reference_seq(grant, n_pages);
}

static void xen_grant_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return;

	for_each_sg(sg, s, nents, i)
		xen_grant_dma_unmap_page(dev, s->dma_address, sg_dma_len(s), dir,
				attrs);
}

static int xen_grant_dma_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return -EINVAL;

	for_each_sg(sg, s, nents, i) {
		s->dma_address = xen_grant_dma_map_page(dev, sg_page(s), s->offset,
				s->length, dir, attrs);
		if (s->dma_address == DMA_MAPPING_ERROR)
			goto out;

		sg_dma_len(s) = s->length;
	}

	return nents;

out:
	xen_grant_dma_unmap_sg(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_dma_len(sg) = 0;

	return -EIO;
}

static int xen_grant_dma_supported(struct device *dev, u64 mask)
{
	return mask == DMA_BIT_MASK(64);
}

static const struct dma_map_ops xen_grant_dma_ops = {
	.alloc = xen_grant_dma_alloc,
	.free = xen_grant_dma_free,
	.alloc_pages = xen_grant_dma_alloc_pages,
	.free_pages = xen_grant_dma_free_pages,
	.mmap = dma_common_mmap,
	.get_sgtable = dma_common_get_sgtable,
	.map_page = xen_grant_dma_map_page,
	.unmap_page = xen_grant_dma_unmap_page,
	.map_sg = xen_grant_dma_map_sg,
	.unmap_sg = xen_grant_dma_unmap_sg,
	.dma_supported = xen_grant_dma_supported,
};

static struct device_node *xen_dt_get_node(struct device *dev)
{
	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);
		struct pci_bus *bus = pdev->bus;

		/* Walk up to the root bus to look for PCI Host controller */
		while (!pci_is_root_bus(bus))
			bus = bus->parent;

		if (!bus->bridge->parent)
			return NULL;
		return of_node_get(bus->bridge->parent->of_node);
	}

	return of_node_get(dev->of_node);
}

static int xen_dt_grant_init_backend_domid(struct device *dev,
					   struct device_node *np,
					   domid_t *backend_domid)
{
	struct of_phandle_args iommu_spec = { .args_count = 1 };

	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);
		u32 rid = PCI_DEVID(pdev->bus->number, pdev->devfn);

		if (of_map_id(np, rid, "iommu-map", "iommu-map-mask", &iommu_spec.np,
				iommu_spec.args)) {
			dev_dbg(dev, "Cannot translate ID\n");
			return -ESRCH;
		}
	} else {
		if (of_parse_phandle_with_args(np, "iommus", "#iommu-cells",
				0, &iommu_spec)) {
			dev_dbg(dev, "Cannot parse iommus property\n");
			return -ESRCH;
		}
	}

	if (!of_device_is_compatible(iommu_spec.np, "xen,grant-dma") ||
			iommu_spec.args_count != 1) {
		dev_dbg(dev, "Incompatible IOMMU node\n");
		of_node_put(iommu_spec.np);
		return -ESRCH;
	}

	of_node_put(iommu_spec.np);

	/*
	 * The endpoint ID here means the ID of the domain where the
	 * corresponding backend is running
	 */
	*backend_domid = iommu_spec.args[0];

	return 0;
}

static int xen_grant_init_backend_domid(struct device *dev,
					domid_t *backend_domid)
{
	struct device_node *np;
	int ret = -ENODEV;

	np = xen_dt_get_node(dev);
	if (np) {
		ret = xen_dt_grant_init_backend_domid(dev, np, backend_domid);
		of_node_put(np);
	} else if (IS_ENABLED(CONFIG_XEN_VIRTIO_FORCE_GRANT) || xen_pv_domain()) {
		dev_info(dev, "Using dom0 as backend\n");
		*backend_domid = 0;
		ret = 0;
	}

	return ret;
}

static void xen_grant_setup_dma_ops(struct device *dev, domid_t backend_domid)
{
	struct xen_grant_dma_data *data;

	data = find_xen_grant_dma_data(dev);
	if (data) {
		dev_err(dev, "Xen grant DMA data is already created\n");
		return;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		goto err;

	data->backend_domid = backend_domid;

	if (store_xen_grant_dma_data(dev, data)) {
		dev_err(dev, "Cannot store Xen grant DMA data\n");
		goto err;
	}

	dev->dma_ops = &xen_grant_dma_ops;

	return;

err:
	devm_kfree(dev, data);
	dev_err(dev, "Cannot set up Xen grant DMA ops, retain platform DMA ops\n");
}

bool xen_virtio_restricted_mem_acc(struct virtio_device *dev)
{
	domid_t backend_domid;

	if (!xen_grant_init_backend_domid(dev->dev.parent, &backend_domid)) {
		xen_grant_setup_dma_ops(dev->dev.parent, backend_domid);
		return true;
	}

	return false;
}

MODULE_DESCRIPTION("Xen grant DMA-mapping layer");
MODULE_AUTHOR("Juergen Gross <jgross@suse.com>");
MODULE_LICENSE("GPL");
