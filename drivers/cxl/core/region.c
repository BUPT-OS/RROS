// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/memregion.h>
#include <linux/genalloc.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/sort.h>
#include <linux/idr.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

/**
 * DOC: cxl core region
 *
 * CXL Regions represent mapped memory capacity in system physical address
 * space. Whereas the CXL Root Decoders identify the bounds of potential CXL
 * Memory ranges, Regions represent the active mapped capacity by the HDM
 * Decoder Capability structures throughout the Host Bridges, Switches, and
 * Endpoints in the topology.
 *
 * Region configuration has ordering constraints. UUID may be set at any time
 * but is only visible for persistent regions.
 * 1. Interleave granularity
 * 2. Interleave size
 * 3. Decoder targets
 */

/*
 * All changes to the interleave configuration occur with this lock held
 * for write.
 */
static DECLARE_RWSEM(cxl_region_rwsem);

static struct cxl_region *to_cxl_region(struct device *dev);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	if (cxlr->mode != CXL_DECODER_PMEM)
		rc = sysfs_emit(buf, "\n");
	else
		rc = sysfs_emit(buf, "%pUb\n", &p->uuid);
	up_read(&cxl_region_rwsem);

	return rc;
}

static int is_dup(struct device *match, void *data)
{
	struct cxl_region_params *p;
	struct cxl_region *cxlr;
	uuid_t *uuid = data;

	if (!is_cxl_region(match))
		return 0;

	lockdep_assert_held(&cxl_region_rwsem);
	cxlr = to_cxl_region(match);
	p = &cxlr->params;

	if (uuid_equal(&p->uuid, uuid)) {
		dev_dbg(match, "already has uuid: %pUb\n", uuid);
		return -EBUSY;
	}

	return 0;
}

static ssize_t uuid_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	uuid_t temp;
	ssize_t rc;

	if (len != UUID_STRING_LEN + 1)
		return -EINVAL;

	rc = uuid_parse(buf, &temp);
	if (rc)
		return rc;

	if (uuid_is_null(&temp))
		return -EINVAL;

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;

	if (uuid_equal(&p->uuid, &temp))
		goto out;

	rc = -EBUSY;
	if (p->state >= CXL_CONFIG_ACTIVE)
		goto out;

	rc = bus_for_each_dev(&cxl_bus_type, NULL, &temp, is_dup);
	if (rc < 0)
		goto out;

	uuid_copy(&p->uuid, &temp);
out:
	up_write(&cxl_region_rwsem);

	if (rc)
		return rc;
	return len;
}
static DEVICE_ATTR_RW(uuid);

static struct cxl_region_ref *cxl_rr_load(struct cxl_port *port,
					  struct cxl_region *cxlr)
{
	return xa_load(&port->regions, (unsigned long)cxlr);
}

static int cxl_region_invalidate_memregion(struct cxl_region *cxlr)
{
	if (!cpu_cache_has_invalidate_memregion()) {
		if (IS_ENABLED(CONFIG_CXL_REGION_INVALIDATION_TEST)) {
			dev_warn_once(
				&cxlr->dev,
				"Bypassing cpu_cache_invalidate_memregion() for testing!\n");
			return 0;
		} else {
			dev_err(&cxlr->dev,
				"Failed to synchronize CPU cache state\n");
			return -ENXIO;
		}
	}

	cpu_cache_invalidate_memregion(IORES_DESC_CXL);
	return 0;
}

static int cxl_region_decode_reset(struct cxl_region *cxlr, int count)
{
	struct cxl_region_params *p = &cxlr->params;
	int i, rc = 0;

	/*
	 * Before region teardown attempt to flush, and if the flush
	 * fails cancel the region teardown for data consistency
	 * concerns
	 */
	rc = cxl_region_invalidate_memregion(cxlr);
	if (rc)
		return rc;

	for (i = count - 1; i >= 0; i--) {
		struct cxl_endpoint_decoder *cxled = p->targets[i];
		struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
		struct cxl_port *iter = cxled_to_port(cxled);
		struct cxl_dev_state *cxlds = cxlmd->cxlds;
		struct cxl_ep *ep;

		if (cxlds->rcd)
			goto endpoint_reset;

		while (!is_cxl_root(to_cxl_port(iter->dev.parent)))
			iter = to_cxl_port(iter->dev.parent);

		for (ep = cxl_ep_load(iter, cxlmd); iter;
		     iter = ep->next, ep = cxl_ep_load(iter, cxlmd)) {
			struct cxl_region_ref *cxl_rr;
			struct cxl_decoder *cxld;

			cxl_rr = cxl_rr_load(iter, cxlr);
			cxld = cxl_rr->decoder;
			if (cxld->reset)
				rc = cxld->reset(cxld);
			if (rc)
				return rc;
			set_bit(CXL_REGION_F_NEEDS_RESET, &cxlr->flags);
		}

endpoint_reset:
		rc = cxled->cxld.reset(&cxled->cxld);
		if (rc)
			return rc;
		set_bit(CXL_REGION_F_NEEDS_RESET, &cxlr->flags);
	}

	/* all decoders associated with this region have been torn down */
	clear_bit(CXL_REGION_F_NEEDS_RESET, &cxlr->flags);

	return 0;
}

static int commit_decoder(struct cxl_decoder *cxld)
{
	struct cxl_switch_decoder *cxlsd = NULL;

	if (cxld->commit)
		return cxld->commit(cxld);

	if (is_switch_decoder(&cxld->dev))
		cxlsd = to_cxl_switch_decoder(&cxld->dev);

	if (dev_WARN_ONCE(&cxld->dev, !cxlsd || cxlsd->nr_targets > 1,
			  "->commit() is required\n"))
		return -ENXIO;
	return 0;
}

static int cxl_region_decode_commit(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	int i, rc = 0;

	for (i = 0; i < p->nr_targets; i++) {
		struct cxl_endpoint_decoder *cxled = p->targets[i];
		struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
		struct cxl_region_ref *cxl_rr;
		struct cxl_decoder *cxld;
		struct cxl_port *iter;
		struct cxl_ep *ep;

		/* commit bottom up */
		for (iter = cxled_to_port(cxled); !is_cxl_root(iter);
		     iter = to_cxl_port(iter->dev.parent)) {
			cxl_rr = cxl_rr_load(iter, cxlr);
			cxld = cxl_rr->decoder;
			rc = commit_decoder(cxld);
			if (rc)
				break;
		}

		if (rc) {
			/* programming @iter failed, teardown */
			for (ep = cxl_ep_load(iter, cxlmd); ep && iter;
			     iter = ep->next, ep = cxl_ep_load(iter, cxlmd)) {
				cxl_rr = cxl_rr_load(iter, cxlr);
				cxld = cxl_rr->decoder;
				if (cxld->reset)
					cxld->reset(cxld);
			}

			cxled->cxld.reset(&cxled->cxld);
			goto err;
		}
	}

	return 0;

err:
	/* undo the targets that were successfully committed */
	cxl_region_decode_reset(cxlr, i);
	return rc;
}

static ssize_t commit_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	bool commit;
	ssize_t rc;

	rc = kstrtobool(buf, &commit);
	if (rc)
		return rc;

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;

	/* Already in the requested state? */
	if (commit && p->state >= CXL_CONFIG_COMMIT)
		goto out;
	if (!commit && p->state < CXL_CONFIG_COMMIT)
		goto out;

	/* Not ready to commit? */
	if (commit && p->state < CXL_CONFIG_ACTIVE) {
		rc = -ENXIO;
		goto out;
	}

	/*
	 * Invalidate caches before region setup to drop any speculative
	 * consumption of this address space
	 */
	rc = cxl_region_invalidate_memregion(cxlr);
	if (rc)
		return rc;

	if (commit) {
		rc = cxl_region_decode_commit(cxlr);
		if (rc == 0)
			p->state = CXL_CONFIG_COMMIT;
	} else {
		p->state = CXL_CONFIG_RESET_PENDING;
		up_write(&cxl_region_rwsem);
		device_release_driver(&cxlr->dev);
		down_write(&cxl_region_rwsem);

		/*
		 * The lock was dropped, so need to revalidate that the reset is
		 * still pending.
		 */
		if (p->state == CXL_CONFIG_RESET_PENDING) {
			rc = cxl_region_decode_reset(cxlr, p->interleave_ways);
			/*
			 * Revert to committed since there may still be active
			 * decoders associated with this region, or move forward
			 * to active to mark the reset successful
			 */
			if (rc)
				p->state = CXL_CONFIG_COMMIT;
			else
				p->state = CXL_CONFIG_ACTIVE;
		}
	}

out:
	up_write(&cxl_region_rwsem);

	if (rc)
		return rc;
	return len;
}

static ssize_t commit_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	rc = sysfs_emit(buf, "%d\n", p->state >= CXL_CONFIG_COMMIT);
	up_read(&cxl_region_rwsem);

	return rc;
}
static DEVICE_ATTR_RW(commit);

static umode_t cxl_region_visible(struct kobject *kobj, struct attribute *a,
				  int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_region *cxlr = to_cxl_region(dev);

	/*
	 * Support tooling that expects to find a 'uuid' attribute for all
	 * regions regardless of mode.
	 */
	if (a == &dev_attr_uuid.attr && cxlr->mode != CXL_DECODER_PMEM)
		return 0444;
	return a->mode;
}

static ssize_t interleave_ways_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	rc = sysfs_emit(buf, "%d\n", p->interleave_ways);
	up_read(&cxl_region_rwsem);

	return rc;
}

static const struct attribute_group *get_cxl_region_target_group(void);

static ssize_t interleave_ways_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev->parent);
	struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	unsigned int val, save;
	int rc;
	u8 iw;

	rc = kstrtouint(buf, 0, &val);
	if (rc)
		return rc;

	rc = ways_to_eiw(val, &iw);
	if (rc)
		return rc;

	/*
	 * Even for x3, x9, and x12 interleaves the region interleave must be a
	 * power of 2 multiple of the host bridge interleave.
	 */
	if (!is_power_of_2(val / cxld->interleave_ways) ||
	    (val % cxld->interleave_ways)) {
		dev_dbg(&cxlr->dev, "invalid interleave: %d\n", val);
		return -EINVAL;
	}

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;
	if (p->state >= CXL_CONFIG_INTERLEAVE_ACTIVE) {
		rc = -EBUSY;
		goto out;
	}

	save = p->interleave_ways;
	p->interleave_ways = val;
	rc = sysfs_update_group(&cxlr->dev.kobj, get_cxl_region_target_group());
	if (rc)
		p->interleave_ways = save;
out:
	up_write(&cxl_region_rwsem);
	if (rc)
		return rc;
	return len;
}
static DEVICE_ATTR_RW(interleave_ways);

static ssize_t interleave_granularity_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	rc = sysfs_emit(buf, "%d\n", p->interleave_granularity);
	up_read(&cxl_region_rwsem);

	return rc;
}

static ssize_t interleave_granularity_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev->parent);
	struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	int rc, val;
	u16 ig;

	rc = kstrtoint(buf, 0, &val);
	if (rc)
		return rc;

	rc = granularity_to_eig(val, &ig);
	if (rc)
		return rc;

	/*
	 * When the host-bridge is interleaved, disallow region granularity !=
	 * root granularity. Regions with a granularity less than the root
	 * interleave result in needing multiple endpoints to support a single
	 * slot in the interleave (possible to support in the future). Regions
	 * with a granularity greater than the root interleave result in invalid
	 * DPA translations (invalid to support).
	 */
	if (cxld->interleave_ways > 1 && val != cxld->interleave_granularity)
		return -EINVAL;

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;
	if (p->state >= CXL_CONFIG_INTERLEAVE_ACTIVE) {
		rc = -EBUSY;
		goto out;
	}

	p->interleave_granularity = val;
out:
	up_write(&cxl_region_rwsem);
	if (rc)
		return rc;
	return len;
}
static DEVICE_ATTR_RW(interleave_granularity);

static ssize_t resource_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	u64 resource = -1ULL;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	if (p->res)
		resource = p->res->start;
	rc = sysfs_emit(buf, "%#llx\n", resource);
	up_read(&cxl_region_rwsem);

	return rc;
}
static DEVICE_ATTR_RO(resource);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%s\n", cxl_decoder_mode_name(cxlr->mode));
}
static DEVICE_ATTR_RO(mode);

static int alloc_hpa(struct cxl_region *cxlr, resource_size_t size)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxlr->dev.parent);
	struct cxl_region_params *p = &cxlr->params;
	struct resource *res;
	u32 remainder = 0;

	lockdep_assert_held_write(&cxl_region_rwsem);

	/* Nothing to do... */
	if (p->res && resource_size(p->res) == size)
		return 0;

	/* To change size the old size must be freed first */
	if (p->res)
		return -EBUSY;

	if (p->state >= CXL_CONFIG_INTERLEAVE_ACTIVE)
		return -EBUSY;

	/* ways, granularity and uuid (if PMEM) need to be set before HPA */
	if (!p->interleave_ways || !p->interleave_granularity ||
	    (cxlr->mode == CXL_DECODER_PMEM && uuid_is_null(&p->uuid)))
		return -ENXIO;

	div_u64_rem(size, SZ_256M * p->interleave_ways, &remainder);
	if (remainder)
		return -EINVAL;

	res = alloc_free_mem_region(cxlrd->res, size, SZ_256M,
				    dev_name(&cxlr->dev));
	if (IS_ERR(res)) {
		dev_dbg(&cxlr->dev, "failed to allocate HPA: %ld\n",
			PTR_ERR(res));
		return PTR_ERR(res);
	}

	p->res = res;
	p->state = CXL_CONFIG_INTERLEAVE_ACTIVE;

	return 0;
}

static void cxl_region_iomem_release(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;

	if (device_is_registered(&cxlr->dev))
		lockdep_assert_held_write(&cxl_region_rwsem);
	if (p->res) {
		/*
		 * Autodiscovered regions may not have been able to insert their
		 * resource.
		 */
		if (p->res->parent)
			remove_resource(p->res);
		kfree(p->res);
		p->res = NULL;
	}
}

static int free_hpa(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;

	lockdep_assert_held_write(&cxl_region_rwsem);

	if (!p->res)
		return 0;

	if (p->state >= CXL_CONFIG_ACTIVE)
		return -EBUSY;

	cxl_region_iomem_release(cxlr);
	p->state = CXL_CONFIG_IDLE;
	return 0;
}

static ssize_t size_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	u64 val;
	int rc;

	rc = kstrtou64(buf, 0, &val);
	if (rc)
		return rc;

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;

	if (val)
		rc = alloc_hpa(cxlr, val);
	else
		rc = free_hpa(cxlr);
	up_write(&cxl_region_rwsem);

	if (rc)
		return rc;

	return len;
}

static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	u64 size = 0;
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;
	if (p->res)
		size = resource_size(p->res);
	rc = sysfs_emit(buf, "%#llx\n", size);
	up_read(&cxl_region_rwsem);

	return rc;
}
static DEVICE_ATTR_RW(size);

static struct attribute *cxl_region_attrs[] = {
	&dev_attr_uuid.attr,
	&dev_attr_commit.attr,
	&dev_attr_interleave_ways.attr,
	&dev_attr_interleave_granularity.attr,
	&dev_attr_resource.attr,
	&dev_attr_size.attr,
	&dev_attr_mode.attr,
	NULL,
};

static const struct attribute_group cxl_region_group = {
	.attrs = cxl_region_attrs,
	.is_visible = cxl_region_visible,
};

static size_t show_targetN(struct cxl_region *cxlr, char *buf, int pos)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled;
	int rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;

	if (pos >= p->interleave_ways) {
		dev_dbg(&cxlr->dev, "position %d out of range %d\n", pos,
			p->interleave_ways);
		rc = -ENXIO;
		goto out;
	}

	cxled = p->targets[pos];
	if (!cxled)
		rc = sysfs_emit(buf, "\n");
	else
		rc = sysfs_emit(buf, "%s\n", dev_name(&cxled->cxld.dev));
out:
	up_read(&cxl_region_rwsem);

	return rc;
}

static int match_free_decoder(struct device *dev, void *data)
{
	struct cxl_decoder *cxld;
	int *id = data;

	if (!is_switch_decoder(dev))
		return 0;

	cxld = to_cxl_decoder(dev);

	/* enforce ordered allocation */
	if (cxld->id != *id)
		return 0;

	if (!cxld->region)
		return 1;

	(*id)++;

	return 0;
}

static int match_auto_decoder(struct device *dev, void *data)
{
	struct cxl_region_params *p = data;
	struct cxl_decoder *cxld;
	struct range *r;

	if (!is_switch_decoder(dev))
		return 0;

	cxld = to_cxl_decoder(dev);
	r = &cxld->hpa_range;

	if (p->res && p->res->start == r->start && p->res->end == r->end)
		return 1;

	return 0;
}

static struct cxl_decoder *cxl_region_find_decoder(struct cxl_port *port,
						   struct cxl_region *cxlr)
{
	struct device *dev;
	int id = 0;

	if (test_bit(CXL_REGION_F_AUTO, &cxlr->flags))
		dev = device_find_child(&port->dev, &cxlr->params,
					match_auto_decoder);
	else
		dev = device_find_child(&port->dev, &id, match_free_decoder);
	if (!dev)
		return NULL;
	/*
	 * This decoder is pinned registered as long as the endpoint decoder is
	 * registered, and endpoint decoder unregistration holds the
	 * cxl_region_rwsem over unregister events, so no need to hold on to
	 * this extra reference.
	 */
	put_device(dev);
	return to_cxl_decoder(dev);
}

static struct cxl_region_ref *alloc_region_ref(struct cxl_port *port,
					       struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_region_ref *cxl_rr, *iter;
	unsigned long index;
	int rc;

	xa_for_each(&port->regions, index, iter) {
		struct cxl_region_params *ip = &iter->region->params;

		if (!ip->res)
			continue;

		if (ip->res->start > p->res->start) {
			dev_dbg(&cxlr->dev,
				"%s: HPA order violation %s:%pr vs %pr\n",
				dev_name(&port->dev),
				dev_name(&iter->region->dev), ip->res, p->res);
			return ERR_PTR(-EBUSY);
		}
	}

	cxl_rr = kzalloc(sizeof(*cxl_rr), GFP_KERNEL);
	if (!cxl_rr)
		return ERR_PTR(-ENOMEM);
	cxl_rr->port = port;
	cxl_rr->region = cxlr;
	cxl_rr->nr_targets = 1;
	xa_init(&cxl_rr->endpoints);

	rc = xa_insert(&port->regions, (unsigned long)cxlr, cxl_rr, GFP_KERNEL);
	if (rc) {
		dev_dbg(&cxlr->dev,
			"%s: failed to track region reference: %d\n",
			dev_name(&port->dev), rc);
		kfree(cxl_rr);
		return ERR_PTR(rc);
	}

	return cxl_rr;
}

static void cxl_rr_free_decoder(struct cxl_region_ref *cxl_rr)
{
	struct cxl_region *cxlr = cxl_rr->region;
	struct cxl_decoder *cxld = cxl_rr->decoder;

	if (!cxld)
		return;

	dev_WARN_ONCE(&cxlr->dev, cxld->region != cxlr, "region mismatch\n");
	if (cxld->region == cxlr) {
		cxld->region = NULL;
		put_device(&cxlr->dev);
	}
}

static void free_region_ref(struct cxl_region_ref *cxl_rr)
{
	struct cxl_port *port = cxl_rr->port;
	struct cxl_region *cxlr = cxl_rr->region;

	cxl_rr_free_decoder(cxl_rr);
	xa_erase(&port->regions, (unsigned long)cxlr);
	xa_destroy(&cxl_rr->endpoints);
	kfree(cxl_rr);
}

static int cxl_rr_ep_add(struct cxl_region_ref *cxl_rr,
			 struct cxl_endpoint_decoder *cxled)
{
	int rc;
	struct cxl_port *port = cxl_rr->port;
	struct cxl_region *cxlr = cxl_rr->region;
	struct cxl_decoder *cxld = cxl_rr->decoder;
	struct cxl_ep *ep = cxl_ep_load(port, cxled_to_memdev(cxled));

	if (ep) {
		rc = xa_insert(&cxl_rr->endpoints, (unsigned long)cxled, ep,
			       GFP_KERNEL);
		if (rc)
			return rc;
	}
	cxl_rr->nr_eps++;

	if (!cxld->region) {
		cxld->region = cxlr;
		get_device(&cxlr->dev);
	}

	return 0;
}

static int cxl_rr_alloc_decoder(struct cxl_port *port, struct cxl_region *cxlr,
				struct cxl_endpoint_decoder *cxled,
				struct cxl_region_ref *cxl_rr)
{
	struct cxl_decoder *cxld;

	if (port == cxled_to_port(cxled))
		cxld = &cxled->cxld;
	else
		cxld = cxl_region_find_decoder(port, cxlr);
	if (!cxld) {
		dev_dbg(&cxlr->dev, "%s: no decoder available\n",
			dev_name(&port->dev));
		return -EBUSY;
	}

	if (cxld->region) {
		dev_dbg(&cxlr->dev, "%s: %s already attached to %s\n",
			dev_name(&port->dev), dev_name(&cxld->dev),
			dev_name(&cxld->region->dev));
		return -EBUSY;
	}

	/*
	 * Endpoints should already match the region type, but backstop that
	 * assumption with an assertion. Switch-decoders change mapping-type
	 * based on what is mapped when they are assigned to a region.
	 */
	dev_WARN_ONCE(&cxlr->dev,
		      port == cxled_to_port(cxled) &&
			      cxld->target_type != cxlr->type,
		      "%s:%s mismatch decoder type %d -> %d\n",
		      dev_name(&cxled_to_memdev(cxled)->dev),
		      dev_name(&cxld->dev), cxld->target_type, cxlr->type);
	cxld->target_type = cxlr->type;
	cxl_rr->decoder = cxld;
	return 0;
}

/**
 * cxl_port_attach_region() - track a region's interest in a port by endpoint
 * @port: port to add a new region reference 'struct cxl_region_ref'
 * @cxlr: region to attach to @port
 * @cxled: endpoint decoder used to create or further pin a region reference
 * @pos: interleave position of @cxled in @cxlr
 *
 * The attach event is an opportunity to validate CXL decode setup
 * constraints and record metadata needed for programming HDM decoders,
 * in particular decoder target lists.
 *
 * The steps are:
 *
 * - validate that there are no other regions with a higher HPA already
 *   associated with @port
 * - establish a region reference if one is not already present
 *
 *   - additionally allocate a decoder instance that will host @cxlr on
 *     @port
 *
 * - pin the region reference by the endpoint
 * - account for how many entries in @port's target list are needed to
 *   cover all of the added endpoints.
 */
static int cxl_port_attach_region(struct cxl_port *port,
				  struct cxl_region *cxlr,
				  struct cxl_endpoint_decoder *cxled, int pos)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_ep *ep = cxl_ep_load(port, cxlmd);
	struct cxl_region_ref *cxl_rr;
	bool nr_targets_inc = false;
	struct cxl_decoder *cxld;
	unsigned long index;
	int rc = -EBUSY;

	lockdep_assert_held_write(&cxl_region_rwsem);

	cxl_rr = cxl_rr_load(port, cxlr);
	if (cxl_rr) {
		struct cxl_ep *ep_iter;
		int found = 0;

		/*
		 * Walk the existing endpoints that have been attached to
		 * @cxlr at @port and see if they share the same 'next' port
		 * in the downstream direction. I.e. endpoints that share common
		 * upstream switch.
		 */
		xa_for_each(&cxl_rr->endpoints, index, ep_iter) {
			if (ep_iter == ep)
				continue;
			if (ep_iter->next == ep->next) {
				found++;
				break;
			}
		}

		/*
		 * New target port, or @port is an endpoint port that always
		 * accounts its own local decode as a target.
		 */
		if (!found || !ep->next) {
			cxl_rr->nr_targets++;
			nr_targets_inc = true;
		}
	} else {
		cxl_rr = alloc_region_ref(port, cxlr);
		if (IS_ERR(cxl_rr)) {
			dev_dbg(&cxlr->dev,
				"%s: failed to allocate region reference\n",
				dev_name(&port->dev));
			return PTR_ERR(cxl_rr);
		}
		nr_targets_inc = true;

		rc = cxl_rr_alloc_decoder(port, cxlr, cxled, cxl_rr);
		if (rc)
			goto out_erase;
	}
	cxld = cxl_rr->decoder;

	rc = cxl_rr_ep_add(cxl_rr, cxled);
	if (rc) {
		dev_dbg(&cxlr->dev,
			"%s: failed to track endpoint %s:%s reference\n",
			dev_name(&port->dev), dev_name(&cxlmd->dev),
			dev_name(&cxld->dev));
		goto out_erase;
	}

	dev_dbg(&cxlr->dev,
		"%s:%s %s add: %s:%s @ %d next: %s nr_eps: %d nr_targets: %d\n",
		dev_name(port->uport_dev), dev_name(&port->dev),
		dev_name(&cxld->dev), dev_name(&cxlmd->dev),
		dev_name(&cxled->cxld.dev), pos,
		ep ? ep->next ? dev_name(ep->next->uport_dev) :
				      dev_name(&cxlmd->dev) :
			   "none",
		cxl_rr->nr_eps, cxl_rr->nr_targets);

	return 0;
out_erase:
	if (nr_targets_inc)
		cxl_rr->nr_targets--;
	if (cxl_rr->nr_eps == 0)
		free_region_ref(cxl_rr);
	return rc;
}

static void cxl_port_detach_region(struct cxl_port *port,
				   struct cxl_region *cxlr,
				   struct cxl_endpoint_decoder *cxled)
{
	struct cxl_region_ref *cxl_rr;
	struct cxl_ep *ep = NULL;

	lockdep_assert_held_write(&cxl_region_rwsem);

	cxl_rr = cxl_rr_load(port, cxlr);
	if (!cxl_rr)
		return;

	/*
	 * Endpoint ports do not carry cxl_ep references, and they
	 * never target more than one endpoint by definition
	 */
	if (cxl_rr->decoder == &cxled->cxld)
		cxl_rr->nr_eps--;
	else
		ep = xa_erase(&cxl_rr->endpoints, (unsigned long)cxled);
	if (ep) {
		struct cxl_ep *ep_iter;
		unsigned long index;
		int found = 0;

		cxl_rr->nr_eps--;
		xa_for_each(&cxl_rr->endpoints, index, ep_iter) {
			if (ep_iter->next == ep->next) {
				found++;
				break;
			}
		}
		if (!found)
			cxl_rr->nr_targets--;
	}

	if (cxl_rr->nr_eps == 0)
		free_region_ref(cxl_rr);
}

static int check_last_peer(struct cxl_endpoint_decoder *cxled,
			   struct cxl_ep *ep, struct cxl_region_ref *cxl_rr,
			   int distance)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_region *cxlr = cxl_rr->region;
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled_peer;
	struct cxl_port *port = cxl_rr->port;
	struct cxl_memdev *cxlmd_peer;
	struct cxl_ep *ep_peer;
	int pos = cxled->pos;

	/*
	 * If this position wants to share a dport with the last endpoint mapped
	 * then that endpoint, at index 'position - distance', must also be
	 * mapped by this dport.
	 */
	if (pos < distance) {
		dev_dbg(&cxlr->dev, "%s:%s: cannot host %s:%s at %d\n",
			dev_name(port->uport_dev), dev_name(&port->dev),
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev), pos);
		return -ENXIO;
	}
	cxled_peer = p->targets[pos - distance];
	cxlmd_peer = cxled_to_memdev(cxled_peer);
	ep_peer = cxl_ep_load(port, cxlmd_peer);
	if (ep->dport != ep_peer->dport) {
		dev_dbg(&cxlr->dev,
			"%s:%s: %s:%s pos %d mismatched peer %s:%s\n",
			dev_name(port->uport_dev), dev_name(&port->dev),
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev), pos,
			dev_name(&cxlmd_peer->dev),
			dev_name(&cxled_peer->cxld.dev));
		return -ENXIO;
	}

	return 0;
}

static int cxl_port_setup_targets(struct cxl_port *port,
				  struct cxl_region *cxlr,
				  struct cxl_endpoint_decoder *cxled)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxlr->dev.parent);
	int parent_iw, parent_ig, ig, iw, rc, inc = 0, pos = cxled->pos;
	struct cxl_port *parent_port = to_cxl_port(port->dev.parent);
	struct cxl_region_ref *cxl_rr = cxl_rr_load(port, cxlr);
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_ep *ep = cxl_ep_load(port, cxlmd);
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_decoder *cxld = cxl_rr->decoder;
	struct cxl_switch_decoder *cxlsd;
	u16 eig, peig;
	u8 eiw, peiw;

	/*
	 * While root level decoders support x3, x6, x12, switch level
	 * decoders only support powers of 2 up to x16.
	 */
	if (!is_power_of_2(cxl_rr->nr_targets)) {
		dev_dbg(&cxlr->dev, "%s:%s: invalid target count %d\n",
			dev_name(port->uport_dev), dev_name(&port->dev),
			cxl_rr->nr_targets);
		return -EINVAL;
	}

	cxlsd = to_cxl_switch_decoder(&cxld->dev);
	if (cxl_rr->nr_targets_set) {
		int i, distance;

		/*
		 * Passthrough decoders impose no distance requirements between
		 * peers
		 */
		if (cxl_rr->nr_targets == 1)
			distance = 0;
		else
			distance = p->nr_targets / cxl_rr->nr_targets;
		for (i = 0; i < cxl_rr->nr_targets_set; i++)
			if (ep->dport == cxlsd->target[i]) {
				rc = check_last_peer(cxled, ep, cxl_rr,
						     distance);
				if (rc)
					return rc;
				goto out_target_set;
			}
		goto add_target;
	}

	if (is_cxl_root(parent_port)) {
		parent_ig = cxlrd->cxlsd.cxld.interleave_granularity;
		parent_iw = cxlrd->cxlsd.cxld.interleave_ways;
		/*
		 * For purposes of address bit routing, use power-of-2 math for
		 * switch ports.
		 */
		if (!is_power_of_2(parent_iw))
			parent_iw /= 3;
	} else {
		struct cxl_region_ref *parent_rr;
		struct cxl_decoder *parent_cxld;

		parent_rr = cxl_rr_load(parent_port, cxlr);
		parent_cxld = parent_rr->decoder;
		parent_ig = parent_cxld->interleave_granularity;
		parent_iw = parent_cxld->interleave_ways;
	}

	rc = granularity_to_eig(parent_ig, &peig);
	if (rc) {
		dev_dbg(&cxlr->dev, "%s:%s: invalid parent granularity: %d\n",
			dev_name(parent_port->uport_dev),
			dev_name(&parent_port->dev), parent_ig);
		return rc;
	}

	rc = ways_to_eiw(parent_iw, &peiw);
	if (rc) {
		dev_dbg(&cxlr->dev, "%s:%s: invalid parent interleave: %d\n",
			dev_name(parent_port->uport_dev),
			dev_name(&parent_port->dev), parent_iw);
		return rc;
	}

	iw = cxl_rr->nr_targets;
	rc = ways_to_eiw(iw, &eiw);
	if (rc) {
		dev_dbg(&cxlr->dev, "%s:%s: invalid port interleave: %d\n",
			dev_name(port->uport_dev), dev_name(&port->dev), iw);
		return rc;
	}

	/*
	 * Interleave granularity is a multiple of @parent_port granularity.
	 * Multiplier is the parent port interleave ways.
	 */
	rc = granularity_to_eig(parent_ig * parent_iw, &eig);
	if (rc) {
		dev_dbg(&cxlr->dev,
			"%s: invalid granularity calculation (%d * %d)\n",
			dev_name(&parent_port->dev), parent_ig, parent_iw);
		return rc;
	}

	rc = eig_to_granularity(eig, &ig);
	if (rc) {
		dev_dbg(&cxlr->dev, "%s:%s: invalid interleave: %d\n",
			dev_name(port->uport_dev), dev_name(&port->dev),
			256 << eig);
		return rc;
	}

	if (test_bit(CXL_REGION_F_AUTO, &cxlr->flags)) {
		if (cxld->interleave_ways != iw ||
		    cxld->interleave_granularity != ig ||
		    cxld->hpa_range.start != p->res->start ||
		    cxld->hpa_range.end != p->res->end ||
		    ((cxld->flags & CXL_DECODER_F_ENABLE) == 0)) {
			dev_err(&cxlr->dev,
				"%s:%s %s expected iw: %d ig: %d %pr\n",
				dev_name(port->uport_dev), dev_name(&port->dev),
				__func__, iw, ig, p->res);
			dev_err(&cxlr->dev,
				"%s:%s %s got iw: %d ig: %d state: %s %#llx:%#llx\n",
				dev_name(port->uport_dev), dev_name(&port->dev),
				__func__, cxld->interleave_ways,
				cxld->interleave_granularity,
				(cxld->flags & CXL_DECODER_F_ENABLE) ?
					"enabled" :
					"disabled",
				cxld->hpa_range.start, cxld->hpa_range.end);
			return -ENXIO;
		}
	} else {
		cxld->interleave_ways = iw;
		cxld->interleave_granularity = ig;
		cxld->hpa_range = (struct range) {
			.start = p->res->start,
			.end = p->res->end,
		};
	}
	dev_dbg(&cxlr->dev, "%s:%s iw: %d ig: %d\n", dev_name(port->uport_dev),
		dev_name(&port->dev), iw, ig);
add_target:
	if (cxl_rr->nr_targets_set == cxl_rr->nr_targets) {
		dev_dbg(&cxlr->dev,
			"%s:%s: targets full trying to add %s:%s at %d\n",
			dev_name(port->uport_dev), dev_name(&port->dev),
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev), pos);
		return -ENXIO;
	}
	if (test_bit(CXL_REGION_F_AUTO, &cxlr->flags)) {
		if (cxlsd->target[cxl_rr->nr_targets_set] != ep->dport) {
			dev_dbg(&cxlr->dev, "%s:%s: %s expected %s at %d\n",
				dev_name(port->uport_dev), dev_name(&port->dev),
				dev_name(&cxlsd->cxld.dev),
				dev_name(ep->dport->dport_dev),
				cxl_rr->nr_targets_set);
			return -ENXIO;
		}
	} else
		cxlsd->target[cxl_rr->nr_targets_set] = ep->dport;
	inc = 1;
out_target_set:
	cxl_rr->nr_targets_set += inc;
	dev_dbg(&cxlr->dev, "%s:%s target[%d] = %s for %s:%s @ %d\n",
		dev_name(port->uport_dev), dev_name(&port->dev),
		cxl_rr->nr_targets_set - 1, dev_name(ep->dport->dport_dev),
		dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev), pos);

	return 0;
}

static void cxl_port_reset_targets(struct cxl_port *port,
				   struct cxl_region *cxlr)
{
	struct cxl_region_ref *cxl_rr = cxl_rr_load(port, cxlr);
	struct cxl_decoder *cxld;

	/*
	 * After the last endpoint has been detached the entire cxl_rr may now
	 * be gone.
	 */
	if (!cxl_rr)
		return;
	cxl_rr->nr_targets_set = 0;

	cxld = cxl_rr->decoder;
	cxld->hpa_range = (struct range) {
		.start = 0,
		.end = -1,
	};
}

static void cxl_region_teardown_targets(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_dev_state *cxlds;
	struct cxl_memdev *cxlmd;
	struct cxl_port *iter;
	struct cxl_ep *ep;
	int i;

	/*
	 * In the auto-discovery case skip automatic teardown since the
	 * address space is already active
	 */
	if (test_bit(CXL_REGION_F_AUTO, &cxlr->flags))
		return;

	for (i = 0; i < p->nr_targets; i++) {
		cxled = p->targets[i];
		cxlmd = cxled_to_memdev(cxled);
		cxlds = cxlmd->cxlds;

		if (cxlds->rcd)
			continue;

		iter = cxled_to_port(cxled);
		while (!is_cxl_root(to_cxl_port(iter->dev.parent)))
			iter = to_cxl_port(iter->dev.parent);

		for (ep = cxl_ep_load(iter, cxlmd); iter;
		     iter = ep->next, ep = cxl_ep_load(iter, cxlmd))
			cxl_port_reset_targets(iter, cxlr);
	}
}

static int cxl_region_setup_targets(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_dev_state *cxlds;
	int i, rc, rch = 0, vh = 0;
	struct cxl_memdev *cxlmd;
	struct cxl_port *iter;
	struct cxl_ep *ep;

	for (i = 0; i < p->nr_targets; i++) {
		cxled = p->targets[i];
		cxlmd = cxled_to_memdev(cxled);
		cxlds = cxlmd->cxlds;

		/* validate that all targets agree on topology */
		if (!cxlds->rcd) {
			vh++;
		} else {
			rch++;
			continue;
		}

		iter = cxled_to_port(cxled);
		while (!is_cxl_root(to_cxl_port(iter->dev.parent)))
			iter = to_cxl_port(iter->dev.parent);

		/*
		 * Descend the topology tree programming / validating
		 * targets while looking for conflicts.
		 */
		for (ep = cxl_ep_load(iter, cxlmd); iter;
		     iter = ep->next, ep = cxl_ep_load(iter, cxlmd)) {
			rc = cxl_port_setup_targets(iter, cxlr, cxled);
			if (rc) {
				cxl_region_teardown_targets(cxlr);
				return rc;
			}
		}
	}

	if (rch && vh) {
		dev_err(&cxlr->dev, "mismatched CXL topologies detected\n");
		cxl_region_teardown_targets(cxlr);
		return -ENXIO;
	}

	return 0;
}

static int cxl_region_validate_position(struct cxl_region *cxlr,
					struct cxl_endpoint_decoder *cxled,
					int pos)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_region_params *p = &cxlr->params;
	int i;

	if (pos < 0 || pos >= p->interleave_ways) {
		dev_dbg(&cxlr->dev, "position %d out of range %d\n", pos,
			p->interleave_ways);
		return -ENXIO;
	}

	if (p->targets[pos] == cxled)
		return 0;

	if (p->targets[pos]) {
		struct cxl_endpoint_decoder *cxled_target = p->targets[pos];
		struct cxl_memdev *cxlmd_target = cxled_to_memdev(cxled_target);

		dev_dbg(&cxlr->dev, "position %d already assigned to %s:%s\n",
			pos, dev_name(&cxlmd_target->dev),
			dev_name(&cxled_target->cxld.dev));
		return -EBUSY;
	}

	for (i = 0; i < p->interleave_ways; i++) {
		struct cxl_endpoint_decoder *cxled_target;
		struct cxl_memdev *cxlmd_target;

		cxled_target = p->targets[i];
		if (!cxled_target)
			continue;

		cxlmd_target = cxled_to_memdev(cxled_target);
		if (cxlmd_target == cxlmd) {
			dev_dbg(&cxlr->dev,
				"%s already specified at position %d via: %s\n",
				dev_name(&cxlmd->dev), pos,
				dev_name(&cxled_target->cxld.dev));
			return -EBUSY;
		}
	}

	return 0;
}

static int cxl_region_attach_position(struct cxl_region *cxlr,
				      struct cxl_root_decoder *cxlrd,
				      struct cxl_endpoint_decoder *cxled,
				      const struct cxl_dport *dport, int pos)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_port *iter;
	int rc;

	if (cxlrd->calc_hb(cxlrd, pos) != dport) {
		dev_dbg(&cxlr->dev, "%s:%s invalid target position for %s\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			dev_name(&cxlrd->cxlsd.cxld.dev));
		return -ENXIO;
	}

	for (iter = cxled_to_port(cxled); !is_cxl_root(iter);
	     iter = to_cxl_port(iter->dev.parent)) {
		rc = cxl_port_attach_region(iter, cxlr, cxled, pos);
		if (rc)
			goto err;
	}

	return 0;

err:
	for (iter = cxled_to_port(cxled); !is_cxl_root(iter);
	     iter = to_cxl_port(iter->dev.parent))
		cxl_port_detach_region(iter, cxlr, cxled);
	return rc;
}

static int cxl_region_attach_auto(struct cxl_region *cxlr,
				  struct cxl_endpoint_decoder *cxled, int pos)
{
	struct cxl_region_params *p = &cxlr->params;

	if (cxled->state != CXL_DECODER_STATE_AUTO) {
		dev_err(&cxlr->dev,
			"%s: unable to add decoder to autodetected region\n",
			dev_name(&cxled->cxld.dev));
		return -EINVAL;
	}

	if (pos >= 0) {
		dev_dbg(&cxlr->dev, "%s: expected auto position, not %d\n",
			dev_name(&cxled->cxld.dev), pos);
		return -EINVAL;
	}

	if (p->nr_targets >= p->interleave_ways) {
		dev_err(&cxlr->dev, "%s: no more target slots available\n",
			dev_name(&cxled->cxld.dev));
		return -ENXIO;
	}

	/*
	 * Temporarily record the endpoint decoder into the target array. Yes,
	 * this means that userspace can view devices in the wrong position
	 * before the region activates, and must be careful to understand when
	 * it might be racing region autodiscovery.
	 */
	pos = p->nr_targets;
	p->targets[pos] = cxled;
	cxled->pos = pos;
	p->nr_targets++;

	return 0;
}

static struct cxl_port *next_port(struct cxl_port *port)
{
	if (!port->parent_dport)
		return NULL;
	return port->parent_dport->port;
}

static int decoder_match_range(struct device *dev, void *data)
{
	struct cxl_endpoint_decoder *cxled = data;
	struct cxl_switch_decoder *cxlsd;

	if (!is_switch_decoder(dev))
		return 0;

	cxlsd = to_cxl_switch_decoder(dev);
	return range_contains(&cxlsd->cxld.hpa_range, &cxled->cxld.hpa_range);
}

static void find_positions(const struct cxl_switch_decoder *cxlsd,
			   const struct cxl_port *iter_a,
			   const struct cxl_port *iter_b, int *a_pos,
			   int *b_pos)
{
	int i;

	for (i = 0, *a_pos = -1, *b_pos = -1; i < cxlsd->nr_targets; i++) {
		if (cxlsd->target[i] == iter_a->parent_dport)
			*a_pos = i;
		else if (cxlsd->target[i] == iter_b->parent_dport)
			*b_pos = i;
		if (*a_pos >= 0 && *b_pos >= 0)
			break;
	}
}

static int cmp_decode_pos(const void *a, const void *b)
{
	struct cxl_endpoint_decoder *cxled_a = *(typeof(cxled_a) *)a;
	struct cxl_endpoint_decoder *cxled_b = *(typeof(cxled_b) *)b;
	struct cxl_memdev *cxlmd_a = cxled_to_memdev(cxled_a);
	struct cxl_memdev *cxlmd_b = cxled_to_memdev(cxled_b);
	struct cxl_port *port_a = cxled_to_port(cxled_a);
	struct cxl_port *port_b = cxled_to_port(cxled_b);
	struct cxl_port *iter_a, *iter_b, *port = NULL;
	struct cxl_switch_decoder *cxlsd;
	struct device *dev;
	int a_pos, b_pos;
	unsigned int seq;

	/* Exit early if any prior sorting failed */
	if (cxled_a->pos < 0 || cxled_b->pos < 0)
		return 0;

	/*
	 * Walk up the hierarchy to find a shared port, find the decoder that
	 * maps the range, compare the relative position of those dport
	 * mappings.
	 */
	for (iter_a = port_a; iter_a; iter_a = next_port(iter_a)) {
		struct cxl_port *next_a, *next_b;

		next_a = next_port(iter_a);
		if (!next_a)
			break;

		for (iter_b = port_b; iter_b; iter_b = next_port(iter_b)) {
			next_b = next_port(iter_b);
			if (next_a != next_b)
				continue;
			port = next_a;
			break;
		}

		if (port)
			break;
	}

	if (!port) {
		dev_err(cxlmd_a->dev.parent,
			"failed to find shared port with %s\n",
			dev_name(cxlmd_b->dev.parent));
		goto err;
	}

	dev = device_find_child(&port->dev, cxled_a, decoder_match_range);
	if (!dev) {
		struct range *range = &cxled_a->cxld.hpa_range;

		dev_err(port->uport_dev,
			"failed to find decoder that maps %#llx-%#llx\n",
			range->start, range->end);
		goto err;
	}

	cxlsd = to_cxl_switch_decoder(dev);
	do {
		seq = read_seqbegin(&cxlsd->target_lock);
		find_positions(cxlsd, iter_a, iter_b, &a_pos, &b_pos);
	} while (read_seqretry(&cxlsd->target_lock, seq));

	put_device(dev);

	if (a_pos < 0 || b_pos < 0) {
		dev_err(port->uport_dev,
			"failed to find shared decoder for %s and %s\n",
			dev_name(cxlmd_a->dev.parent),
			dev_name(cxlmd_b->dev.parent));
		goto err;
	}

	dev_dbg(port->uport_dev, "%s comes %s %s\n",
		dev_name(cxlmd_a->dev.parent),
		a_pos - b_pos < 0 ? "before" : "after",
		dev_name(cxlmd_b->dev.parent));

	return a_pos - b_pos;
err:
	cxled_a->pos = -1;
	return 0;
}

static int cxl_region_sort_targets(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	int i, rc = 0;

	sort(p->targets, p->nr_targets, sizeof(p->targets[0]), cmp_decode_pos,
	     NULL);

	for (i = 0; i < p->nr_targets; i++) {
		struct cxl_endpoint_decoder *cxled = p->targets[i];

		/*
		 * Record that sorting failed, but still continue to restore
		 * cxled->pos with its ->targets[] position so that follow-on
		 * code paths can reliably do p->targets[cxled->pos] to
		 * self-reference their entry.
		 */
		if (cxled->pos < 0)
			rc = -ENXIO;
		cxled->pos = i;
	}

	dev_dbg(&cxlr->dev, "region sort %s\n", rc ? "failed" : "successful");
	return rc;
}

static int cxl_region_attach(struct cxl_region *cxlr,
			     struct cxl_endpoint_decoder *cxled, int pos)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(cxlr->dev.parent);
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_port *ep_port, *root_port;
	struct cxl_dport *dport;
	int rc = -ENXIO;

	if (cxled->mode != cxlr->mode) {
		dev_dbg(&cxlr->dev, "%s region mode: %d mismatch: %d\n",
			dev_name(&cxled->cxld.dev), cxlr->mode, cxled->mode);
		return -EINVAL;
	}

	if (cxled->mode == CXL_DECODER_DEAD) {
		dev_dbg(&cxlr->dev, "%s dead\n", dev_name(&cxled->cxld.dev));
		return -ENODEV;
	}

	/* all full of members, or interleave config not established? */
	if (p->state > CXL_CONFIG_INTERLEAVE_ACTIVE) {
		dev_dbg(&cxlr->dev, "region already active\n");
		return -EBUSY;
	} else if (p->state < CXL_CONFIG_INTERLEAVE_ACTIVE) {
		dev_dbg(&cxlr->dev, "interleave config missing\n");
		return -ENXIO;
	}

	ep_port = cxled_to_port(cxled);
	root_port = cxlrd_to_port(cxlrd);
	dport = cxl_find_dport_by_dev(root_port, ep_port->host_bridge);
	if (!dport) {
		dev_dbg(&cxlr->dev, "%s:%s invalid target for %s\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			dev_name(cxlr->dev.parent));
		return -ENXIO;
	}

	if (cxled->cxld.target_type != cxlr->type) {
		dev_dbg(&cxlr->dev, "%s:%s type mismatch: %d vs %d\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			cxled->cxld.target_type, cxlr->type);
		return -ENXIO;
	}

	if (!cxled->dpa_res) {
		dev_dbg(&cxlr->dev, "%s:%s: missing DPA allocation.\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev));
		return -ENXIO;
	}

	if (resource_size(cxled->dpa_res) * p->interleave_ways !=
	    resource_size(p->res)) {
		dev_dbg(&cxlr->dev,
			"%s:%s: decoder-size-%#llx * ways-%d != region-size-%#llx\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			(u64)resource_size(cxled->dpa_res), p->interleave_ways,
			(u64)resource_size(p->res));
		return -EINVAL;
	}

	if (test_bit(CXL_REGION_F_AUTO, &cxlr->flags)) {
		int i;

		rc = cxl_region_attach_auto(cxlr, cxled, pos);
		if (rc)
			return rc;

		/* await more targets to arrive... */
		if (p->nr_targets < p->interleave_ways)
			return 0;

		/*
		 * All targets are here, which implies all PCI enumeration that
		 * affects this region has been completed. Walk the topology to
		 * sort the devices into their relative region decode position.
		 */
		rc = cxl_region_sort_targets(cxlr);
		if (rc)
			return rc;

		for (i = 0; i < p->nr_targets; i++) {
			cxled = p->targets[i];
			ep_port = cxled_to_port(cxled);
			dport = cxl_find_dport_by_dev(root_port,
						      ep_port->host_bridge);
			rc = cxl_region_attach_position(cxlr, cxlrd, cxled,
							dport, i);
			if (rc)
				return rc;
		}

		rc = cxl_region_setup_targets(cxlr);
		if (rc)
			return rc;

		/*
		 * If target setup succeeds in the autodiscovery case
		 * then the region is already committed.
		 */
		p->state = CXL_CONFIG_COMMIT;

		return 0;
	}

	rc = cxl_region_validate_position(cxlr, cxled, pos);
	if (rc)
		return rc;

	rc = cxl_region_attach_position(cxlr, cxlrd, cxled, dport, pos);
	if (rc)
		return rc;

	p->targets[pos] = cxled;
	cxled->pos = pos;
	p->nr_targets++;

	if (p->nr_targets == p->interleave_ways) {
		rc = cxl_region_setup_targets(cxlr);
		if (rc)
			goto err_decrement;
		p->state = CXL_CONFIG_ACTIVE;
	}

	cxled->cxld.interleave_ways = p->interleave_ways;
	cxled->cxld.interleave_granularity = p->interleave_granularity;
	cxled->cxld.hpa_range = (struct range) {
		.start = p->res->start,
		.end = p->res->end,
	};

	return 0;

err_decrement:
	p->nr_targets--;
	cxled->pos = -1;
	p->targets[pos] = NULL;
	return rc;
}

static int cxl_region_detach(struct cxl_endpoint_decoder *cxled)
{
	struct cxl_port *iter, *ep_port = cxled_to_port(cxled);
	struct cxl_region *cxlr = cxled->cxld.region;
	struct cxl_region_params *p;
	int rc = 0;

	lockdep_assert_held_write(&cxl_region_rwsem);

	if (!cxlr)
		return 0;

	p = &cxlr->params;
	get_device(&cxlr->dev);

	if (p->state > CXL_CONFIG_ACTIVE) {
		/*
		 * TODO: tear down all impacted regions if a device is
		 * removed out of order
		 */
		rc = cxl_region_decode_reset(cxlr, p->interleave_ways);
		if (rc)
			goto out;
		p->state = CXL_CONFIG_ACTIVE;
	}

	for (iter = ep_port; !is_cxl_root(iter);
	     iter = to_cxl_port(iter->dev.parent))
		cxl_port_detach_region(iter, cxlr, cxled);

	if (cxled->pos < 0 || cxled->pos >= p->interleave_ways ||
	    p->targets[cxled->pos] != cxled) {
		struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);

		dev_WARN_ONCE(&cxlr->dev, 1, "expected %s:%s at position %d\n",
			      dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			      cxled->pos);
		goto out;
	}

	if (p->state == CXL_CONFIG_ACTIVE) {
		p->state = CXL_CONFIG_INTERLEAVE_ACTIVE;
		cxl_region_teardown_targets(cxlr);
	}
	p->targets[cxled->pos] = NULL;
	p->nr_targets--;
	cxled->cxld.hpa_range = (struct range) {
		.start = 0,
		.end = -1,
	};

	/* notify the region driver that one of its targets has departed */
	up_write(&cxl_region_rwsem);
	device_release_driver(&cxlr->dev);
	down_write(&cxl_region_rwsem);
out:
	put_device(&cxlr->dev);
	return rc;
}

void cxl_decoder_kill_region(struct cxl_endpoint_decoder *cxled)
{
	down_write(&cxl_region_rwsem);
	cxled->mode = CXL_DECODER_DEAD;
	cxl_region_detach(cxled);
	up_write(&cxl_region_rwsem);
}

static int attach_target(struct cxl_region *cxlr,
			 struct cxl_endpoint_decoder *cxled, int pos,
			 unsigned int state)
{
	int rc = 0;

	if (state == TASK_INTERRUPTIBLE)
		rc = down_write_killable(&cxl_region_rwsem);
	else
		down_write(&cxl_region_rwsem);
	if (rc)
		return rc;

	down_read(&cxl_dpa_rwsem);
	rc = cxl_region_attach(cxlr, cxled, pos);
	up_read(&cxl_dpa_rwsem);
	up_write(&cxl_region_rwsem);
	return rc;
}

static int detach_target(struct cxl_region *cxlr, int pos)
{
	struct cxl_region_params *p = &cxlr->params;
	int rc;

	rc = down_write_killable(&cxl_region_rwsem);
	if (rc)
		return rc;

	if (pos >= p->interleave_ways) {
		dev_dbg(&cxlr->dev, "position %d out of range %d\n", pos,
			p->interleave_ways);
		rc = -ENXIO;
		goto out;
	}

	if (!p->targets[pos]) {
		rc = 0;
		goto out;
	}

	rc = cxl_region_detach(p->targets[pos]);
out:
	up_write(&cxl_region_rwsem);
	return rc;
}

static size_t store_targetN(struct cxl_region *cxlr, const char *buf, int pos,
			    size_t len)
{
	int rc;

	if (sysfs_streq(buf, "\n"))
		rc = detach_target(cxlr, pos);
	else {
		struct device *dev;

		dev = bus_find_device_by_name(&cxl_bus_type, NULL, buf);
		if (!dev)
			return -ENODEV;

		if (!is_endpoint_decoder(dev)) {
			rc = -EINVAL;
			goto out;
		}

		rc = attach_target(cxlr, to_cxl_endpoint_decoder(dev), pos,
				   TASK_INTERRUPTIBLE);
out:
		put_device(dev);
	}

	if (rc < 0)
		return rc;
	return len;
}

#define TARGET_ATTR_RW(n)                                              \
static ssize_t target##n##_show(                                       \
	struct device *dev, struct device_attribute *attr, char *buf)  \
{                                                                      \
	return show_targetN(to_cxl_region(dev), buf, (n));             \
}                                                                      \
static ssize_t target##n##_store(struct device *dev,                   \
				 struct device_attribute *attr,        \
				 const char *buf, size_t len)          \
{                                                                      \
	return store_targetN(to_cxl_region(dev), buf, (n), len);       \
}                                                                      \
static DEVICE_ATTR_RW(target##n)

TARGET_ATTR_RW(0);
TARGET_ATTR_RW(1);
TARGET_ATTR_RW(2);
TARGET_ATTR_RW(3);
TARGET_ATTR_RW(4);
TARGET_ATTR_RW(5);
TARGET_ATTR_RW(6);
TARGET_ATTR_RW(7);
TARGET_ATTR_RW(8);
TARGET_ATTR_RW(9);
TARGET_ATTR_RW(10);
TARGET_ATTR_RW(11);
TARGET_ATTR_RW(12);
TARGET_ATTR_RW(13);
TARGET_ATTR_RW(14);
TARGET_ATTR_RW(15);

static struct attribute *target_attrs[] = {
	&dev_attr_target0.attr,
	&dev_attr_target1.attr,
	&dev_attr_target2.attr,
	&dev_attr_target3.attr,
	&dev_attr_target4.attr,
	&dev_attr_target5.attr,
	&dev_attr_target6.attr,
	&dev_attr_target7.attr,
	&dev_attr_target8.attr,
	&dev_attr_target9.attr,
	&dev_attr_target10.attr,
	&dev_attr_target11.attr,
	&dev_attr_target12.attr,
	&dev_attr_target13.attr,
	&dev_attr_target14.attr,
	&dev_attr_target15.attr,
	NULL,
};

static umode_t cxl_region_target_visible(struct kobject *kobj,
					 struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;

	if (n < p->interleave_ways)
		return a->mode;
	return 0;
}

static const struct attribute_group cxl_region_target_group = {
	.attrs = target_attrs,
	.is_visible = cxl_region_target_visible,
};

static const struct attribute_group *get_cxl_region_target_group(void)
{
	return &cxl_region_target_group;
}

static const struct attribute_group *region_groups[] = {
	&cxl_base_attribute_group,
	&cxl_region_group,
	&cxl_region_target_group,
	NULL,
};

static void cxl_region_release(struct device *dev)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev->parent);
	struct cxl_region *cxlr = to_cxl_region(dev);
	int id = atomic_read(&cxlrd->region_id);

	/*
	 * Try to reuse the recently idled id rather than the cached
	 * next id to prevent the region id space from increasing
	 * unnecessarily.
	 */
	if (cxlr->id < id)
		if (atomic_try_cmpxchg(&cxlrd->region_id, &id, cxlr->id)) {
			memregion_free(id);
			goto out;
		}

	memregion_free(cxlr->id);
out:
	put_device(dev->parent);
	kfree(cxlr);
}

const struct device_type cxl_region_type = {
	.name = "cxl_region",
	.release = cxl_region_release,
	.groups = region_groups
};

bool is_cxl_region(struct device *dev)
{
	return dev->type == &cxl_region_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_region, CXL);

static struct cxl_region *to_cxl_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, dev->type != &cxl_region_type,
			  "not a cxl_region device\n"))
		return NULL;

	return container_of(dev, struct cxl_region, dev);
}

static void unregister_region(void *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	int i;

	device_del(dev);

	/*
	 * Now that region sysfs is shutdown, the parameter block is now
	 * read-only, so no need to hold the region rwsem to access the
	 * region parameters.
	 */
	for (i = 0; i < p->interleave_ways; i++)
		detach_target(cxlr, i);

	cxl_region_iomem_release(cxlr);
	put_device(dev);
}

static struct lock_class_key cxl_region_key;

static struct cxl_region *cxl_region_alloc(struct cxl_root_decoder *cxlrd, int id)
{
	struct cxl_region *cxlr;
	struct device *dev;

	cxlr = kzalloc(sizeof(*cxlr), GFP_KERNEL);
	if (!cxlr) {
		memregion_free(id);
		return ERR_PTR(-ENOMEM);
	}

	dev = &cxlr->dev;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_region_key);
	dev->parent = &cxlrd->cxlsd.cxld.dev;
	/*
	 * Keep root decoder pinned through cxl_region_release to fixup
	 * region id allocations
	 */
	get_device(dev->parent);
	device_set_pm_not_required(dev);
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_region_type;
	cxlr->id = id;

	return cxlr;
}

/**
 * devm_cxl_add_region - Adds a region to a decoder
 * @cxlrd: root decoder
 * @id: memregion id to create, or memregion_free() on failure
 * @mode: mode for the endpoint decoders of this region
 * @type: select whether this is an expander or accelerator (type-2 or type-3)
 *
 * This is the second step of region initialization. Regions exist within an
 * address space which is mapped by a @cxlrd.
 *
 * Return: 0 if the region was added to the @cxlrd, else returns negative error
 * code. The region will be named "regionZ" where Z is the unique region number.
 */
static struct cxl_region *devm_cxl_add_region(struct cxl_root_decoder *cxlrd,
					      int id,
					      enum cxl_decoder_mode mode,
					      enum cxl_decoder_type type)
{
	struct cxl_port *port = to_cxl_port(cxlrd->cxlsd.cxld.dev.parent);
	struct cxl_region *cxlr;
	struct device *dev;
	int rc;

	switch (mode) {
	case CXL_DECODER_RAM:
	case CXL_DECODER_PMEM:
		break;
	default:
		dev_err(&cxlrd->cxlsd.cxld.dev, "unsupported mode %d\n", mode);
		return ERR_PTR(-EINVAL);
	}

	cxlr = cxl_region_alloc(cxlrd, id);
	if (IS_ERR(cxlr))
		return cxlr;
	cxlr->mode = mode;
	cxlr->type = type;

	dev = &cxlr->dev;
	rc = dev_set_name(dev, "region%d", id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	rc = devm_add_action_or_reset(port->uport_dev, unregister_region, cxlr);
	if (rc)
		return ERR_PTR(rc);

	dev_dbg(port->uport_dev, "%s: created %s\n",
		dev_name(&cxlrd->cxlsd.cxld.dev), dev_name(dev));
	return cxlr;

err:
	put_device(dev);
	return ERR_PTR(rc);
}

static ssize_t __create_region_show(struct cxl_root_decoder *cxlrd, char *buf)
{
	return sysfs_emit(buf, "region%u\n", atomic_read(&cxlrd->region_id));
}

static ssize_t create_pmem_region_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return __create_region_show(to_cxl_root_decoder(dev), buf);
}

static ssize_t create_ram_region_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return __create_region_show(to_cxl_root_decoder(dev), buf);
}

static struct cxl_region *__create_region(struct cxl_root_decoder *cxlrd,
					  enum cxl_decoder_mode mode, int id)
{
	int rc;

	rc = memregion_alloc(GFP_KERNEL);
	if (rc < 0)
		return ERR_PTR(rc);

	if (atomic_cmpxchg(&cxlrd->region_id, id, rc) != id) {
		memregion_free(rc);
		return ERR_PTR(-EBUSY);
	}

	return devm_cxl_add_region(cxlrd, id, mode, CXL_DECODER_HOSTONLYMEM);
}

static ssize_t create_pmem_region_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev);
	struct cxl_region *cxlr;
	int rc, id;

	rc = sscanf(buf, "region%d\n", &id);
	if (rc != 1)
		return -EINVAL;

	cxlr = __create_region(cxlrd, CXL_DECODER_PMEM, id);
	if (IS_ERR(cxlr))
		return PTR_ERR(cxlr);

	return len;
}
DEVICE_ATTR_RW(create_pmem_region);

static ssize_t create_ram_region_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev);
	struct cxl_region *cxlr;
	int rc, id;

	rc = sscanf(buf, "region%d\n", &id);
	if (rc != 1)
		return -EINVAL;

	cxlr = __create_region(cxlrd, CXL_DECODER_RAM, id);
	if (IS_ERR(cxlr))
		return PTR_ERR(cxlr);

	return len;
}
DEVICE_ATTR_RW(create_ram_region);

static ssize_t region_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cxl_decoder *cxld = to_cxl_decoder(dev);
	ssize_t rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;

	if (cxld->region)
		rc = sysfs_emit(buf, "%s\n", dev_name(&cxld->region->dev));
	else
		rc = sysfs_emit(buf, "\n");
	up_read(&cxl_region_rwsem);

	return rc;
}
DEVICE_ATTR_RO(region);

static struct cxl_region *
cxl_find_region_by_name(struct cxl_root_decoder *cxlrd, const char *name)
{
	struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
	struct device *region_dev;

	region_dev = device_find_child_by_name(&cxld->dev, name);
	if (!region_dev)
		return ERR_PTR(-ENODEV);

	return to_cxl_region(region_dev);
}

static ssize_t delete_region_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev);
	struct cxl_port *port = to_cxl_port(dev->parent);
	struct cxl_region *cxlr;

	cxlr = cxl_find_region_by_name(cxlrd, buf);
	if (IS_ERR(cxlr))
		return PTR_ERR(cxlr);

	devm_release_action(port->uport_dev, unregister_region, cxlr);
	put_device(&cxlr->dev);

	return len;
}
DEVICE_ATTR_WO(delete_region);

static void cxl_pmem_region_release(struct device *dev)
{
	struct cxl_pmem_region *cxlr_pmem = to_cxl_pmem_region(dev);
	int i;

	for (i = 0; i < cxlr_pmem->nr_mappings; i++) {
		struct cxl_memdev *cxlmd = cxlr_pmem->mapping[i].cxlmd;

		put_device(&cxlmd->dev);
	}

	kfree(cxlr_pmem);
}

static const struct attribute_group *cxl_pmem_region_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL,
};

const struct device_type cxl_pmem_region_type = {
	.name = "cxl_pmem_region",
	.release = cxl_pmem_region_release,
	.groups = cxl_pmem_region_attribute_groups,
};

bool is_cxl_pmem_region(struct device *dev)
{
	return dev->type == &cxl_pmem_region_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_pmem_region, CXL);

struct cxl_pmem_region *to_cxl_pmem_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_pmem_region(dev),
			  "not a cxl_pmem_region device\n"))
		return NULL;
	return container_of(dev, struct cxl_pmem_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_pmem_region, CXL);

struct cxl_poison_context {
	struct cxl_port *port;
	enum cxl_decoder_mode mode;
	u64 offset;
};

static int cxl_get_poison_unmapped(struct cxl_memdev *cxlmd,
				   struct cxl_poison_context *ctx)
{
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	u64 offset, length;
	int rc = 0;

	/*
	 * Collect poison for the remaining unmapped resources
	 * after poison is collected by committed endpoints.
	 *
	 * Knowing that PMEM must always follow RAM, get poison
	 * for unmapped resources based on the last decoder's mode:
	 *	ram: scan remains of ram range, then any pmem range
	 *	pmem: scan remains of pmem range
	 */

	if (ctx->mode == CXL_DECODER_RAM) {
		offset = ctx->offset;
		length = resource_size(&cxlds->ram_res) - offset;
		rc = cxl_mem_get_poison(cxlmd, offset, length, NULL);
		if (rc == -EFAULT)
			rc = 0;
		if (rc)
			return rc;
	}
	if (ctx->mode == CXL_DECODER_PMEM) {
		offset = ctx->offset;
		length = resource_size(&cxlds->dpa_res) - offset;
		if (!length)
			return 0;
	} else if (resource_size(&cxlds->pmem_res)) {
		offset = cxlds->pmem_res.start;
		length = resource_size(&cxlds->pmem_res);
	} else {
		return 0;
	}

	return cxl_mem_get_poison(cxlmd, offset, length, NULL);
}

static int poison_by_decoder(struct device *dev, void *arg)
{
	struct cxl_poison_context *ctx = arg;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_memdev *cxlmd;
	u64 offset, length;
	int rc = 0;

	if (!is_endpoint_decoder(dev))
		return rc;

	cxled = to_cxl_endpoint_decoder(dev);
	if (!cxled->dpa_res || !resource_size(cxled->dpa_res))
		return rc;

	/*
	 * Regions are only created with single mode decoders: pmem or ram.
	 * Linux does not support mixed mode decoders. This means that
	 * reading poison per endpoint decoder adheres to the requirement
	 * that poison reads of pmem and ram must be separated.
	 * CXL 3.0 Spec 8.2.9.8.4.1
	 */
	if (cxled->mode == CXL_DECODER_MIXED) {
		dev_dbg(dev, "poison list read unsupported in mixed mode\n");
		return rc;
	}

	cxlmd = cxled_to_memdev(cxled);
	if (cxled->skip) {
		offset = cxled->dpa_res->start - cxled->skip;
		length = cxled->skip;
		rc = cxl_mem_get_poison(cxlmd, offset, length, NULL);
		if (rc == -EFAULT && cxled->mode == CXL_DECODER_RAM)
			rc = 0;
		if (rc)
			return rc;
	}

	offset = cxled->dpa_res->start;
	length = cxled->dpa_res->end - offset + 1;
	rc = cxl_mem_get_poison(cxlmd, offset, length, cxled->cxld.region);
	if (rc == -EFAULT && cxled->mode == CXL_DECODER_RAM)
		rc = 0;
	if (rc)
		return rc;

	/* Iterate until commit_end is reached */
	if (cxled->cxld.id == ctx->port->commit_end) {
		ctx->offset = cxled->dpa_res->end + 1;
		ctx->mode = cxled->mode;
		return 1;
	}

	return 0;
}

int cxl_get_poison_by_endpoint(struct cxl_port *port)
{
	struct cxl_poison_context ctx;
	int rc = 0;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc)
		return rc;

	ctx = (struct cxl_poison_context) {
		.port = port
	};

	rc = device_for_each_child(&port->dev, &ctx, poison_by_decoder);
	if (rc == 1)
		rc = cxl_get_poison_unmapped(to_cxl_memdev(port->uport_dev),
					     &ctx);

	up_read(&cxl_region_rwsem);
	return rc;
}

static struct lock_class_key cxl_pmem_region_key;

static struct cxl_pmem_region *cxl_pmem_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct cxl_pmem_region *cxlr_pmem;
	struct device *dev;
	int i;

	down_read(&cxl_region_rwsem);
	if (p->state != CXL_CONFIG_COMMIT) {
		cxlr_pmem = ERR_PTR(-ENXIO);
		goto out;
	}

	cxlr_pmem = kzalloc(struct_size(cxlr_pmem, mapping, p->nr_targets),
			    GFP_KERNEL);
	if (!cxlr_pmem) {
		cxlr_pmem = ERR_PTR(-ENOMEM);
		goto out;
	}

	cxlr_pmem->hpa_range.start = p->res->start;
	cxlr_pmem->hpa_range.end = p->res->end;

	/* Snapshot the region configuration underneath the cxl_region_rwsem */
	cxlr_pmem->nr_mappings = p->nr_targets;
	for (i = 0; i < p->nr_targets; i++) {
		struct cxl_endpoint_decoder *cxled = p->targets[i];
		struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
		struct cxl_pmem_region_mapping *m = &cxlr_pmem->mapping[i];

		/*
		 * Regions never span CXL root devices, so by definition the
		 * bridge for one device is the same for all.
		 */
		if (i == 0) {
			cxl_nvb = cxl_find_nvdimm_bridge(cxlmd);
			if (!cxl_nvb) {
				cxlr_pmem = ERR_PTR(-ENODEV);
				goto out;
			}
			cxlr->cxl_nvb = cxl_nvb;
		}
		m->cxlmd = cxlmd;
		get_device(&cxlmd->dev);
		m->start = cxled->dpa_res->start;
		m->size = resource_size(cxled->dpa_res);
		m->position = i;
	}

	dev = &cxlr_pmem->dev;
	cxlr_pmem->cxlr = cxlr;
	cxlr->cxlr_pmem = cxlr_pmem;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_pmem_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_pmem_region_type;
out:
	up_read(&cxl_region_rwsem);

	return cxlr_pmem;
}

static void cxl_dax_region_release(struct device *dev)
{
	struct cxl_dax_region *cxlr_dax = to_cxl_dax_region(dev);

	kfree(cxlr_dax);
}

static const struct attribute_group *cxl_dax_region_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL,
};

const struct device_type cxl_dax_region_type = {
	.name = "cxl_dax_region",
	.release = cxl_dax_region_release,
	.groups = cxl_dax_region_attribute_groups,
};

static bool is_cxl_dax_region(struct device *dev)
{
	return dev->type == &cxl_dax_region_type;
}

struct cxl_dax_region *to_cxl_dax_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_dax_region(dev),
			  "not a cxl_dax_region device\n"))
		return NULL;
	return container_of(dev, struct cxl_dax_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_dax_region, CXL);

static struct lock_class_key cxl_dax_region_key;

static struct cxl_dax_region *cxl_dax_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;

	down_read(&cxl_region_rwsem);
	if (p->state != CXL_CONFIG_COMMIT) {
		cxlr_dax = ERR_PTR(-ENXIO);
		goto out;
	}

	cxlr_dax = kzalloc(sizeof(*cxlr_dax), GFP_KERNEL);
	if (!cxlr_dax) {
		cxlr_dax = ERR_PTR(-ENOMEM);
		goto out;
	}

	cxlr_dax->hpa_range.start = p->res->start;
	cxlr_dax->hpa_range.end = p->res->end;

	dev = &cxlr_dax->dev;
	cxlr_dax->cxlr = cxlr;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_dax_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_dax_region_type;
out:
	up_read(&cxl_region_rwsem);

	return cxlr_dax;
}

static void cxlr_pmem_unregister(void *_cxlr_pmem)
{
	struct cxl_pmem_region *cxlr_pmem = _cxlr_pmem;
	struct cxl_region *cxlr = cxlr_pmem->cxlr;
	struct cxl_nvdimm_bridge *cxl_nvb = cxlr->cxl_nvb;

	/*
	 * Either the bridge is in ->remove() context under the device_lock(),
	 * or cxlr_release_nvdimm() is cancelling the bridge's release action
	 * for @cxlr_pmem and doing it itself (while manually holding the bridge
	 * lock).
	 */
	device_lock_assert(&cxl_nvb->dev);
	cxlr->cxlr_pmem = NULL;
	cxlr_pmem->cxlr = NULL;
	device_unregister(&cxlr_pmem->dev);
}

static void cxlr_release_nvdimm(void *_cxlr)
{
	struct cxl_region *cxlr = _cxlr;
	struct cxl_nvdimm_bridge *cxl_nvb = cxlr->cxl_nvb;

	device_lock(&cxl_nvb->dev);
	if (cxlr->cxlr_pmem)
		devm_release_action(&cxl_nvb->dev, cxlr_pmem_unregister,
				    cxlr->cxlr_pmem);
	device_unlock(&cxl_nvb->dev);
	cxlr->cxl_nvb = NULL;
	put_device(&cxl_nvb->dev);
}

/**
 * devm_cxl_add_pmem_region() - add a cxl_region-to-nd_region bridge
 * @cxlr: parent CXL region for this pmem region bridge device
 *
 * Return: 0 on success negative error code on failure.
 */
static int devm_cxl_add_pmem_region(struct cxl_region *cxlr)
{
	struct cxl_pmem_region *cxlr_pmem;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *dev;
	int rc;

	cxlr_pmem = cxl_pmem_region_alloc(cxlr);
	if (IS_ERR(cxlr_pmem))
		return PTR_ERR(cxlr_pmem);
	cxl_nvb = cxlr->cxl_nvb;

	dev = &cxlr_pmem->dev;
	rc = dev_set_name(dev, "pmem_region%d", cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	device_lock(&cxl_nvb->dev);
	if (cxl_nvb->dev.driver)
		rc = devm_add_action_or_reset(&cxl_nvb->dev,
					      cxlr_pmem_unregister, cxlr_pmem);
	else
		rc = -ENXIO;
	device_unlock(&cxl_nvb->dev);

	if (rc)
		goto err_bridge;

	/* @cxlr carries a reference on @cxl_nvb until cxlr_release_nvdimm */
	return devm_add_action_or_reset(&cxlr->dev, cxlr_release_nvdimm, cxlr);

err:
	put_device(dev);
err_bridge:
	put_device(&cxl_nvb->dev);
	cxlr->cxl_nvb = NULL;
	return rc;
}

static void cxlr_dax_unregister(void *_cxlr_dax)
{
	struct cxl_dax_region *cxlr_dax = _cxlr_dax;

	device_unregister(&cxlr_dax->dev);
}

static int devm_cxl_add_dax_region(struct cxl_region *cxlr)
{
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;
	int rc;

	cxlr_dax = cxl_dax_region_alloc(cxlr);
	if (IS_ERR(cxlr_dax))
		return PTR_ERR(cxlr_dax);

	dev = &cxlr_dax->dev;
	rc = dev_set_name(dev, "dax_region%d", cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	return devm_add_action_or_reset(&cxlr->dev, cxlr_dax_unregister,
					cxlr_dax);
err:
	put_device(dev);
	return rc;
}

static int match_decoder_by_range(struct device *dev, void *data)
{
	struct range *r1, *r2 = data;
	struct cxl_root_decoder *cxlrd;

	if (!is_root_decoder(dev))
		return 0;

	cxlrd = to_cxl_root_decoder(dev);
	r1 = &cxlrd->cxlsd.cxld.hpa_range;
	return range_contains(r1, r2);
}

static int match_region_by_range(struct device *dev, void *data)
{
	struct cxl_region_params *p;
	struct cxl_region *cxlr;
	struct range *r = data;
	int rc = 0;

	if (!is_cxl_region(dev))
		return 0;

	cxlr = to_cxl_region(dev);
	p = &cxlr->params;

	down_read(&cxl_region_rwsem);
	if (p->res && p->res->start == r->start && p->res->end == r->end)
		rc = 1;
	up_read(&cxl_region_rwsem);

	return rc;
}

/* Establish an empty region covering the given HPA range */
static struct cxl_region *construct_region(struct cxl_root_decoder *cxlrd,
					   struct cxl_endpoint_decoder *cxled)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_port *port = cxlrd_to_port(cxlrd);
	struct range *hpa = &cxled->cxld.hpa_range;
	struct cxl_region_params *p;
	struct cxl_region *cxlr;
	struct resource *res;
	int rc;

	do {
		cxlr = __create_region(cxlrd, cxled->mode,
				       atomic_read(&cxlrd->region_id));
	} while (IS_ERR(cxlr) && PTR_ERR(cxlr) == -EBUSY);

	if (IS_ERR(cxlr)) {
		dev_err(cxlmd->dev.parent,
			"%s:%s: %s failed assign region: %ld\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			__func__, PTR_ERR(cxlr));
		return cxlr;
	}

	down_write(&cxl_region_rwsem);
	p = &cxlr->params;
	if (p->state >= CXL_CONFIG_INTERLEAVE_ACTIVE) {
		dev_err(cxlmd->dev.parent,
			"%s:%s: %s autodiscovery interrupted\n",
			dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			__func__);
		rc = -EBUSY;
		goto err;
	}

	set_bit(CXL_REGION_F_AUTO, &cxlr->flags);

	res = kmalloc(sizeof(*res), GFP_KERNEL);
	if (!res) {
		rc = -ENOMEM;
		goto err;
	}

	*res = DEFINE_RES_MEM_NAMED(hpa->start, range_len(hpa),
				    dev_name(&cxlr->dev));
	rc = insert_resource(cxlrd->res, res);
	if (rc) {
		/*
		 * Platform-firmware may not have split resources like "System
		 * RAM" on CXL window boundaries see cxl_region_iomem_release()
		 */
		dev_warn(cxlmd->dev.parent,
			 "%s:%s: %s %s cannot insert resource\n",
			 dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev),
			 __func__, dev_name(&cxlr->dev));
	}

	p->res = res;
	p->interleave_ways = cxled->cxld.interleave_ways;
	p->interleave_granularity = cxled->cxld.interleave_granularity;
	p->state = CXL_CONFIG_INTERLEAVE_ACTIVE;

	rc = sysfs_update_group(&cxlr->dev.kobj, get_cxl_region_target_group());
	if (rc)
		goto err;

	dev_dbg(cxlmd->dev.parent, "%s:%s: %s %s res: %pr iw: %d ig: %d\n",
		dev_name(&cxlmd->dev), dev_name(&cxled->cxld.dev), __func__,
		dev_name(&cxlr->dev), p->res, p->interleave_ways,
		p->interleave_granularity);

	/* ...to match put_device() in cxl_add_to_region() */
	get_device(&cxlr->dev);
	up_write(&cxl_region_rwsem);

	return cxlr;

err:
	up_write(&cxl_region_rwsem);
	devm_release_action(port->uport_dev, unregister_region, cxlr);
	return ERR_PTR(rc);
}

int cxl_add_to_region(struct cxl_port *root, struct cxl_endpoint_decoder *cxled)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct range *hpa = &cxled->cxld.hpa_range;
	struct cxl_decoder *cxld = &cxled->cxld;
	struct device *cxlrd_dev, *region_dev;
	struct cxl_root_decoder *cxlrd;
	struct cxl_region_params *p;
	struct cxl_region *cxlr;
	bool attach = false;
	int rc;

	cxlrd_dev = device_find_child(&root->dev, &cxld->hpa_range,
				      match_decoder_by_range);
	if (!cxlrd_dev) {
		dev_err(cxlmd->dev.parent,
			"%s:%s no CXL window for range %#llx:%#llx\n",
			dev_name(&cxlmd->dev), dev_name(&cxld->dev),
			cxld->hpa_range.start, cxld->hpa_range.end);
		return -ENXIO;
	}

	cxlrd = to_cxl_root_decoder(cxlrd_dev);

	/*
	 * Ensure that if multiple threads race to construct_region() for @hpa
	 * one does the construction and the others add to that.
	 */
	mutex_lock(&cxlrd->range_lock);
	region_dev = device_find_child(&cxlrd->cxlsd.cxld.dev, hpa,
				       match_region_by_range);
	if (!region_dev) {
		cxlr = construct_region(cxlrd, cxled);
		region_dev = &cxlr->dev;
	} else
		cxlr = to_cxl_region(region_dev);
	mutex_unlock(&cxlrd->range_lock);

	rc = PTR_ERR_OR_ZERO(cxlr);
	if (rc)
		goto out;

	attach_target(cxlr, cxled, -1, TASK_UNINTERRUPTIBLE);

	down_read(&cxl_region_rwsem);
	p = &cxlr->params;
	attach = p->state == CXL_CONFIG_COMMIT;
	up_read(&cxl_region_rwsem);

	if (attach) {
		/*
		 * If device_attach() fails the range may still be active via
		 * the platform-firmware memory map, otherwise the driver for
		 * regions is local to this file, so driver matching can't fail.
		 */
		if (device_attach(&cxlr->dev) < 0)
			dev_err(&cxlr->dev, "failed to enable, range: %pr\n",
				p->res);
	}

	put_device(region_dev);
out:
	put_device(cxlrd_dev);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(cxl_add_to_region, CXL);

static int is_system_ram(struct resource *res, void *arg)
{
	struct cxl_region *cxlr = arg;
	struct cxl_region_params *p = &cxlr->params;

	dev_dbg(&cxlr->dev, "%pr has System RAM: %pr\n", p->res, res);
	return 1;
}

static int cxl_region_probe(struct device *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	int rc;

	rc = down_read_interruptible(&cxl_region_rwsem);
	if (rc) {
		dev_dbg(&cxlr->dev, "probe interrupted\n");
		return rc;
	}

	if (p->state < CXL_CONFIG_COMMIT) {
		dev_dbg(&cxlr->dev, "config state: %d\n", p->state);
		rc = -ENXIO;
		goto out;
	}

	if (test_bit(CXL_REGION_F_NEEDS_RESET, &cxlr->flags)) {
		dev_err(&cxlr->dev,
			"failed to activate, re-commit region and retry\n");
		rc = -ENXIO;
		goto out;
	}

	/*
	 * From this point on any path that changes the region's state away from
	 * CXL_CONFIG_COMMIT is also responsible for releasing the driver.
	 */
out:
	up_read(&cxl_region_rwsem);

	if (rc)
		return rc;

	switch (cxlr->mode) {
	case CXL_DECODER_PMEM:
		return devm_cxl_add_pmem_region(cxlr);
	case CXL_DECODER_RAM:
		/*
		 * The region can not be manged by CXL if any portion of
		 * it is already online as 'System RAM'
		 */
		if (walk_iomem_res_desc(IORES_DESC_NONE,
					IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
					p->res->start, p->res->end, cxlr,
					is_system_ram) > 0)
			return 0;
		return devm_cxl_add_dax_region(cxlr);
	default:
		dev_dbg(&cxlr->dev, "unsupported region mode: %d\n",
			cxlr->mode);
		return -ENXIO;
	}
}

static struct cxl_driver cxl_region_driver = {
	.name = "cxl_region",
	.probe = cxl_region_probe,
	.id = CXL_DEVICE_REGION,
};

int cxl_region_init(void)
{
	return cxl_driver_register(&cxl_region_driver);
}

void cxl_region_exit(void)
{
	cxl_driver_unregister(&cxl_region_driver);
}

MODULE_IMPORT_NS(CXL);
MODULE_IMPORT_NS(DEVMEM);
MODULE_ALIAS_CXL(CXL_DEVICE_REGION);
