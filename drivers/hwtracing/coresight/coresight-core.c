// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
 */

#include <linux/build_bug.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/stringhash.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/coresight.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>

#include "coresight-etm-perf.h"
#include "coresight-priv.h"
#include "coresight-syscfg.h"

static DEFINE_MUTEX(coresight_mutex);
static DEFINE_PER_CPU(struct coresight_device *, csdev_sink);

/*
 * Use IDR to map the hash of the source's device name
 * to the pointer of path for the source. The idr is for
 * the sources which aren't associated with CPU.
 */
static DEFINE_IDR(path_idr);

/**
 * struct coresight_node - elements of a path, from source to sink
 * @csdev:	Address of an element.
 * @link:	hook to the list.
 */
struct coresight_node {
	struct coresight_device *csdev;
	struct list_head link;
};

/*
 * When operating Coresight drivers from the sysFS interface, only a single
 * path can exist from a tracer (associated to a CPU) to a sink.
 */
static DEFINE_PER_CPU(struct list_head *, tracer_path);

/*
 * When losing synchronisation a new barrier packet needs to be inserted at the
 * beginning of the data collected in a buffer.  That way the decoder knows that
 * it needs to look for another sync sequence.
 */
const u32 coresight_barrier_pkt[4] = {0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff};
EXPORT_SYMBOL_GPL(coresight_barrier_pkt);

static const struct cti_assoc_op *cti_assoc_ops;

ssize_t coresight_simple_show_pair(struct device *_dev,
			      struct device_attribute *attr, char *buf)
{
	struct coresight_device *csdev = container_of(_dev, struct coresight_device, dev);
	struct cs_pair_attribute *cs_attr = container_of(attr, struct cs_pair_attribute, attr);
	u64 val;

	pm_runtime_get_sync(_dev->parent);
	val = csdev_access_relaxed_read_pair(&csdev->access, cs_attr->lo_off, cs_attr->hi_off);
	pm_runtime_put_sync(_dev->parent);
	return sysfs_emit(buf, "0x%llx\n", val);
}
EXPORT_SYMBOL_GPL(coresight_simple_show_pair);

ssize_t coresight_simple_show32(struct device *_dev,
			      struct device_attribute *attr, char *buf)
{
	struct coresight_device *csdev = container_of(_dev, struct coresight_device, dev);
	struct cs_off_attribute *cs_attr = container_of(attr, struct cs_off_attribute, attr);
	u64 val;

	pm_runtime_get_sync(_dev->parent);
	val = csdev_access_relaxed_read32(&csdev->access, cs_attr->off);
	pm_runtime_put_sync(_dev->parent);
	return sysfs_emit(buf, "0x%llx\n", val);
}
EXPORT_SYMBOL_GPL(coresight_simple_show32);

void coresight_set_cti_ops(const struct cti_assoc_op *cti_op)
{
	cti_assoc_ops = cti_op;
}
EXPORT_SYMBOL_GPL(coresight_set_cti_ops);

void coresight_remove_cti_ops(void)
{
	cti_assoc_ops = NULL;
}
EXPORT_SYMBOL_GPL(coresight_remove_cti_ops);

void coresight_set_percpu_sink(int cpu, struct coresight_device *csdev)
{
	per_cpu(csdev_sink, cpu) = csdev;
}
EXPORT_SYMBOL_GPL(coresight_set_percpu_sink);

struct coresight_device *coresight_get_percpu_sink(int cpu)
{
	return per_cpu(csdev_sink, cpu);
}
EXPORT_SYMBOL_GPL(coresight_get_percpu_sink);

static struct coresight_connection *
coresight_find_out_connection(struct coresight_device *src_dev,
			      struct coresight_device *dest_dev)
{
	int i;
	struct coresight_connection *conn;

	for (i = 0; i < src_dev->pdata->nr_outconns; i++) {
		conn = src_dev->pdata->out_conns[i];
		if (conn->dest_dev == dest_dev)
			return conn;
	}

	dev_err(&src_dev->dev,
		"couldn't find output connection, src_dev: %s, dest_dev: %s\n",
		dev_name(&src_dev->dev), dev_name(&dest_dev->dev));

	return ERR_PTR(-ENODEV);
}

static inline u32 coresight_read_claim_tags(struct coresight_device *csdev)
{
	return csdev_access_relaxed_read32(&csdev->access, CORESIGHT_CLAIMCLR);
}

static inline bool coresight_is_claimed_self_hosted(struct coresight_device *csdev)
{
	return coresight_read_claim_tags(csdev) == CORESIGHT_CLAIM_SELF_HOSTED;
}

static inline bool coresight_is_claimed_any(struct coresight_device *csdev)
{
	return coresight_read_claim_tags(csdev) != 0;
}

static inline void coresight_set_claim_tags(struct coresight_device *csdev)
{
	csdev_access_relaxed_write32(&csdev->access, CORESIGHT_CLAIM_SELF_HOSTED,
				     CORESIGHT_CLAIMSET);
	isb();
}

static inline void coresight_clear_claim_tags(struct coresight_device *csdev)
{
	csdev_access_relaxed_write32(&csdev->access, CORESIGHT_CLAIM_SELF_HOSTED,
				     CORESIGHT_CLAIMCLR);
	isb();
}

/*
 * coresight_claim_device_unlocked : Claim the device for self-hosted usage
 * to prevent an external tool from touching this device. As per PSCI
 * standards, section "Preserving the execution context" => "Debug and Trace
 * save and Restore", DBGCLAIM[1] is reserved for Self-hosted debug/trace and
 * DBGCLAIM[0] is reserved for external tools.
 *
 * Called with CS_UNLOCKed for the component.
 * Returns : 0 on success
 */
int coresight_claim_device_unlocked(struct coresight_device *csdev)
{
	if (WARN_ON(!csdev))
		return -EINVAL;

	if (coresight_is_claimed_any(csdev))
		return -EBUSY;

	coresight_set_claim_tags(csdev);
	if (coresight_is_claimed_self_hosted(csdev))
		return 0;
	/* There was a race setting the tags, clean up and fail */
	coresight_clear_claim_tags(csdev);
	return -EBUSY;
}
EXPORT_SYMBOL_GPL(coresight_claim_device_unlocked);

int coresight_claim_device(struct coresight_device *csdev)
{
	int rc;

	if (WARN_ON(!csdev))
		return -EINVAL;

	CS_UNLOCK(csdev->access.base);
	rc = coresight_claim_device_unlocked(csdev);
	CS_LOCK(csdev->access.base);

	return rc;
}
EXPORT_SYMBOL_GPL(coresight_claim_device);

/*
 * coresight_disclaim_device_unlocked : Clear the claim tags for the device.
 * Called with CS_UNLOCKed for the component.
 */
void coresight_disclaim_device_unlocked(struct coresight_device *csdev)
{

	if (WARN_ON(!csdev))
		return;

	if (coresight_is_claimed_self_hosted(csdev))
		coresight_clear_claim_tags(csdev);
	else
		/*
		 * The external agent may have not honoured our claim
		 * and has manipulated it. Or something else has seriously
		 * gone wrong in our driver.
		 */
		WARN_ON_ONCE(1);
}
EXPORT_SYMBOL_GPL(coresight_disclaim_device_unlocked);

void coresight_disclaim_device(struct coresight_device *csdev)
{
	if (WARN_ON(!csdev))
		return;

	CS_UNLOCK(csdev->access.base);
	coresight_disclaim_device_unlocked(csdev);
	CS_LOCK(csdev->access.base);
}
EXPORT_SYMBOL_GPL(coresight_disclaim_device);

/*
 * Add a helper as an output device. This function takes the @coresight_mutex
 * because it's assumed that it's called from the helper device, outside of the
 * core code where the mutex would already be held. Don't add new calls to this
 * from inside the core code, instead try to add the new helper to the DT and
 * ACPI where it will be picked up and linked automatically.
 */
void coresight_add_helper(struct coresight_device *csdev,
			  struct coresight_device *helper)
{
	int i;
	struct coresight_connection conn = {};
	struct coresight_connection *new_conn;

	mutex_lock(&coresight_mutex);
	conn.dest_fwnode = fwnode_handle_get(dev_fwnode(&helper->dev));
	conn.dest_dev = helper;
	conn.dest_port = conn.src_port = -1;
	conn.src_dev = csdev;

	/*
	 * Check for duplicates because this is called every time a helper
	 * device is re-loaded. Existing connections will get re-linked
	 * automatically.
	 */
	for (i = 0; i < csdev->pdata->nr_outconns; ++i)
		if (csdev->pdata->out_conns[i]->dest_fwnode == conn.dest_fwnode)
			goto unlock;

	new_conn = coresight_add_out_conn(csdev->dev.parent, csdev->pdata,
					  &conn);
	if (!IS_ERR(new_conn))
		coresight_add_in_conn(new_conn);

unlock:
	mutex_unlock(&coresight_mutex);
}
EXPORT_SYMBOL_GPL(coresight_add_helper);

static int coresight_enable_sink(struct coresight_device *csdev,
				 enum cs_mode mode, void *data)
{
	int ret;

	/*
	 * We need to make sure the "new" session is compatible with the
	 * existing "mode" of operation.
	 */
	if (!sink_ops(csdev)->enable)
		return -EINVAL;

	ret = sink_ops(csdev)->enable(csdev, mode, data);
	if (ret)
		return ret;

	csdev->enable = true;

	return 0;
}

static void coresight_disable_sink(struct coresight_device *csdev)
{
	int ret;

	if (!sink_ops(csdev)->disable)
		return;

	ret = sink_ops(csdev)->disable(csdev);
	if (ret)
		return;
	csdev->enable = false;
}

static int coresight_enable_link(struct coresight_device *csdev,
				 struct coresight_device *parent,
				 struct coresight_device *child)
{
	int ret = 0;
	int link_subtype;
	struct coresight_connection *inconn, *outconn;

	if (!parent || !child)
		return -EINVAL;

	inconn = coresight_find_out_connection(parent, csdev);
	outconn = coresight_find_out_connection(csdev, child);
	link_subtype = csdev->subtype.link_subtype;

	if (link_subtype == CORESIGHT_DEV_SUBTYPE_LINK_MERG && IS_ERR(inconn))
		return PTR_ERR(inconn);
	if (link_subtype == CORESIGHT_DEV_SUBTYPE_LINK_SPLIT && IS_ERR(outconn))
		return PTR_ERR(outconn);

	if (link_ops(csdev)->enable) {
		ret = link_ops(csdev)->enable(csdev, inconn, outconn);
		if (!ret)
			csdev->enable = true;
	}

	return ret;
}

static void coresight_disable_link(struct coresight_device *csdev,
				   struct coresight_device *parent,
				   struct coresight_device *child)
{
	int i;
	int link_subtype;
	struct coresight_connection *inconn, *outconn;

	if (!parent || !child)
		return;

	inconn = coresight_find_out_connection(parent, csdev);
	outconn = coresight_find_out_connection(csdev, child);
	link_subtype = csdev->subtype.link_subtype;

	if (link_ops(csdev)->disable) {
		link_ops(csdev)->disable(csdev, inconn, outconn);
	}

	if (link_subtype == CORESIGHT_DEV_SUBTYPE_LINK_MERG) {
		for (i = 0; i < csdev->pdata->nr_inconns; i++)
			if (atomic_read(&csdev->pdata->in_conns[i]->dest_refcnt) !=
			    0)
				return;
	} else if (link_subtype == CORESIGHT_DEV_SUBTYPE_LINK_SPLIT) {
		for (i = 0; i < csdev->pdata->nr_outconns; i++)
			if (atomic_read(&csdev->pdata->out_conns[i]->src_refcnt) !=
			    0)
				return;
	} else {
		if (atomic_read(&csdev->refcnt) != 0)
			return;
	}

	csdev->enable = false;
}

int coresight_enable_source(struct coresight_device *csdev, enum cs_mode mode,
			    void *data)
{
	int ret;

	if (!csdev->enable) {
		if (source_ops(csdev)->enable) {
			ret = source_ops(csdev)->enable(csdev, data, mode);
			if (ret)
				return ret;
		}
		csdev->enable = true;
	}

	atomic_inc(&csdev->refcnt);

	return 0;
}
EXPORT_SYMBOL_GPL(coresight_enable_source);

static bool coresight_is_helper(struct coresight_device *csdev)
{
	return csdev->type == CORESIGHT_DEV_TYPE_HELPER;
}

static int coresight_enable_helper(struct coresight_device *csdev,
				   enum cs_mode mode, void *data)
{
	int ret;

	if (!helper_ops(csdev)->enable)
		return 0;
	ret = helper_ops(csdev)->enable(csdev, mode, data);
	if (ret)
		return ret;

	csdev->enable = true;
	return 0;
}

static void coresight_disable_helper(struct coresight_device *csdev)
{
	int ret;

	if (!helper_ops(csdev)->disable)
		return;

	ret = helper_ops(csdev)->disable(csdev, NULL);
	if (ret)
		return;
	csdev->enable = false;
}

static void coresight_disable_helpers(struct coresight_device *csdev)
{
	int i;
	struct coresight_device *helper;

	for (i = 0; i < csdev->pdata->nr_outconns; ++i) {
		helper = csdev->pdata->out_conns[i]->dest_dev;
		if (helper && coresight_is_helper(helper))
			coresight_disable_helper(helper);
	}
}

/**
 *  coresight_disable_source - Drop the reference count by 1 and disable
 *  the device if there are no users left.
 *
 *  @csdev: The coresight device to disable
 *  @data: Opaque data to pass on to the disable function of the source device.
 *         For example in perf mode this is a pointer to the struct perf_event.
 *
 *  Returns true if the device has been disabled.
 */
bool coresight_disable_source(struct coresight_device *csdev, void *data)
{
	if (atomic_dec_return(&csdev->refcnt) == 0) {
		if (source_ops(csdev)->disable)
			source_ops(csdev)->disable(csdev, data);
		coresight_disable_helpers(csdev);
		csdev->enable = false;
	}
	return !csdev->enable;
}
EXPORT_SYMBOL_GPL(coresight_disable_source);

/*
 * coresight_disable_path_from : Disable components in the given path beyond
 * @nd in the list. If @nd is NULL, all the components, except the SOURCE are
 * disabled.
 */
static void coresight_disable_path_from(struct list_head *path,
					struct coresight_node *nd)
{
	u32 type;
	struct coresight_device *csdev, *parent, *child;

	if (!nd)
		nd = list_first_entry(path, struct coresight_node, link);

	list_for_each_entry_continue(nd, path, link) {
		csdev = nd->csdev;
		type = csdev->type;

		/*
		 * ETF devices are tricky... They can be a link or a sink,
		 * depending on how they are configured.  If an ETF has been
		 * "activated" it will be configured as a sink, otherwise
		 * go ahead with the link configuration.
		 */
		if (type == CORESIGHT_DEV_TYPE_LINKSINK)
			type = (csdev == coresight_get_sink(path)) ?
						CORESIGHT_DEV_TYPE_SINK :
						CORESIGHT_DEV_TYPE_LINK;

		switch (type) {
		case CORESIGHT_DEV_TYPE_SINK:
			coresight_disable_sink(csdev);
			break;
		case CORESIGHT_DEV_TYPE_SOURCE:
			/*
			 * We skip the first node in the path assuming that it
			 * is the source. So we don't expect a source device in
			 * the middle of a path.
			 */
			WARN_ON(1);
			break;
		case CORESIGHT_DEV_TYPE_LINK:
			parent = list_prev_entry(nd, link)->csdev;
			child = list_next_entry(nd, link)->csdev;
			coresight_disable_link(csdev, parent, child);
			break;
		default:
			break;
		}

		/* Disable all helpers adjacent along the path last */
		coresight_disable_helpers(csdev);
	}
}

void coresight_disable_path(struct list_head *path)
{
	coresight_disable_path_from(path, NULL);
}
EXPORT_SYMBOL_GPL(coresight_disable_path);

static int coresight_enable_helpers(struct coresight_device *csdev,
				    enum cs_mode mode, void *data)
{
	int i, ret = 0;
	struct coresight_device *helper;

	for (i = 0; i < csdev->pdata->nr_outconns; ++i) {
		helper = csdev->pdata->out_conns[i]->dest_dev;
		if (!helper || !coresight_is_helper(helper))
			continue;

		ret = coresight_enable_helper(helper, mode, data);
		if (ret)
			return ret;
	}

	return 0;
}

int coresight_enable_path(struct list_head *path, enum cs_mode mode,
			  void *sink_data)
{
	int ret = 0;
	u32 type;
	struct coresight_node *nd;
	struct coresight_device *csdev, *parent, *child;

	list_for_each_entry_reverse(nd, path, link) {
		csdev = nd->csdev;
		type = csdev->type;

		/* Enable all helpers adjacent to the path first */
		ret = coresight_enable_helpers(csdev, mode, sink_data);
		if (ret)
			goto err;
		/*
		 * ETF devices are tricky... They can be a link or a sink,
		 * depending on how they are configured.  If an ETF has been
		 * "activated" it will be configured as a sink, otherwise
		 * go ahead with the link configuration.
		 */
		if (type == CORESIGHT_DEV_TYPE_LINKSINK)
			type = (csdev == coresight_get_sink(path)) ?
						CORESIGHT_DEV_TYPE_SINK :
						CORESIGHT_DEV_TYPE_LINK;

		switch (type) {
		case CORESIGHT_DEV_TYPE_SINK:
			ret = coresight_enable_sink(csdev, mode, sink_data);
			/*
			 * Sink is the first component turned on. If we
			 * failed to enable the sink, there are no components
			 * that need disabling. Disabling the path here
			 * would mean we could disrupt an existing session.
			 */
			if (ret)
				goto out;
			break;
		case CORESIGHT_DEV_TYPE_SOURCE:
			/* sources are enabled from either sysFS or Perf */
			break;
		case CORESIGHT_DEV_TYPE_LINK:
			parent = list_prev_entry(nd, link)->csdev;
			child = list_next_entry(nd, link)->csdev;
			ret = coresight_enable_link(csdev, parent, child);
			if (ret)
				goto err;
			break;
		default:
			goto err;
		}
	}

out:
	return ret;
err:
	coresight_disable_path_from(path, nd);
	goto out;
}

struct coresight_device *coresight_get_sink(struct list_head *path)
{
	struct coresight_device *csdev;

	if (!path)
		return NULL;

	csdev = list_last_entry(path, struct coresight_node, link)->csdev;
	if (csdev->type != CORESIGHT_DEV_TYPE_SINK &&
	    csdev->type != CORESIGHT_DEV_TYPE_LINKSINK)
		return NULL;

	return csdev;
}

static struct coresight_device *
coresight_find_enabled_sink(struct coresight_device *csdev)
{
	int i;
	struct coresight_device *sink = NULL;

	if ((csdev->type == CORESIGHT_DEV_TYPE_SINK ||
	     csdev->type == CORESIGHT_DEV_TYPE_LINKSINK) &&
	     csdev->activated)
		return csdev;

	/*
	 * Recursively explore each port found on this element.
	 */
	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child_dev;

		child_dev = csdev->pdata->out_conns[i]->dest_dev;
		if (child_dev)
			sink = coresight_find_enabled_sink(child_dev);
		if (sink)
			return sink;
	}

	return NULL;
}

/**
 * coresight_get_enabled_sink - returns the first enabled sink using
 * connection based search starting from the source reference
 *
 * @source: Coresight source device reference
 */
struct coresight_device *
coresight_get_enabled_sink(struct coresight_device *source)
{
	if (!source)
		return NULL;

	return coresight_find_enabled_sink(source);
}

static int coresight_sink_by_id(struct device *dev, const void *data)
{
	struct coresight_device *csdev = to_coresight_device(dev);
	unsigned long hash;

	if (csdev->type == CORESIGHT_DEV_TYPE_SINK ||
	     csdev->type == CORESIGHT_DEV_TYPE_LINKSINK) {

		if (!csdev->ea)
			return 0;
		/*
		 * See function etm_perf_add_symlink_sink() to know where
		 * this comes from.
		 */
		hash = (unsigned long)csdev->ea->var;

		if ((u32)hash == *(u32 *)data)
			return 1;
	}

	return 0;
}

/**
 * coresight_get_sink_by_id - returns the sink that matches the id
 * @id: Id of the sink to match
 *
 * The name of a sink is unique, whether it is found on the AMBA bus or
 * otherwise.  As such the hash of that name can easily be used to identify
 * a sink.
 */
struct coresight_device *coresight_get_sink_by_id(u32 id)
{
	struct device *dev = NULL;

	dev = bus_find_device(&coresight_bustype, NULL, &id,
			      coresight_sink_by_id);

	return dev ? to_coresight_device(dev) : NULL;
}

/**
 * coresight_get_ref- Helper function to increase reference count to module
 * and device.
 *
 * @csdev: The coresight device to get a reference on.
 *
 * Return true in successful case and power up the device.
 * Return false when failed to get reference of module.
 */
static inline bool coresight_get_ref(struct coresight_device *csdev)
{
	struct device *dev = csdev->dev.parent;

	/* Make sure the driver can't be removed */
	if (!try_module_get(dev->driver->owner))
		return false;
	/* Make sure the device can't go away */
	get_device(dev);
	pm_runtime_get_sync(dev);
	return true;
}

/**
 * coresight_put_ref- Helper function to decrease reference count to module
 * and device. Power off the device.
 *
 * @csdev: The coresight device to decrement a reference from.
 */
static inline void coresight_put_ref(struct coresight_device *csdev)
{
	struct device *dev = csdev->dev.parent;

	pm_runtime_put(dev);
	put_device(dev);
	module_put(dev->driver->owner);
}

/*
 * coresight_grab_device - Power up this device and any of the helper
 * devices connected to it for trace operation. Since the helper devices
 * don't appear on the trace path, they should be handled along with the
 * master device.
 */
static int coresight_grab_device(struct coresight_device *csdev)
{
	int i;

	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child;

		child = csdev->pdata->out_conns[i]->dest_dev;
		if (child && coresight_is_helper(child))
			if (!coresight_get_ref(child))
				goto err;
	}
	if (coresight_get_ref(csdev))
		return 0;
err:
	for (i--; i >= 0; i--) {
		struct coresight_device *child;

		child = csdev->pdata->out_conns[i]->dest_dev;
		if (child && coresight_is_helper(child))
			coresight_put_ref(child);
	}
	return -ENODEV;
}

/*
 * coresight_drop_device - Release this device and any of the helper
 * devices connected to it.
 */
static void coresight_drop_device(struct coresight_device *csdev)
{
	int i;

	coresight_put_ref(csdev);
	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child;

		child = csdev->pdata->out_conns[i]->dest_dev;
		if (child && coresight_is_helper(child))
			coresight_put_ref(child);
	}
}

/**
 * _coresight_build_path - recursively build a path from a @csdev to a sink.
 * @csdev:	The device to start from.
 * @sink:	The final sink we want in this path.
 * @path:	The list to add devices to.
 *
 * The tree of Coresight device is traversed until an activated sink is
 * found.  From there the sink is added to the list along with all the
 * devices that led to that point - the end result is a list from source
 * to sink. In that list the source is the first device and the sink the
 * last one.
 */
static int _coresight_build_path(struct coresight_device *csdev,
				 struct coresight_device *sink,
				 struct list_head *path)
{
	int i, ret;
	bool found = false;
	struct coresight_node *node;

	/* An activated sink has been found.  Enqueue the element */
	if (csdev == sink)
		goto out;

	if (coresight_is_percpu_source(csdev) && coresight_is_percpu_sink(sink) &&
	    sink == per_cpu(csdev_sink, source_ops(csdev)->cpu_id(csdev))) {
		if (_coresight_build_path(sink, sink, path) == 0) {
			found = true;
			goto out;
		}
	}

	/* Not a sink - recursively explore each port found on this element */
	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child_dev;

		child_dev = csdev->pdata->out_conns[i]->dest_dev;
		if (child_dev &&
		    _coresight_build_path(child_dev, sink, path) == 0) {
			found = true;
			break;
		}
	}

	if (!found)
		return -ENODEV;

out:
	/*
	 * A path from this element to a sink has been found.  The elements
	 * leading to the sink are already enqueued, all that is left to do
	 * is tell the PM runtime core we need this element and add a node
	 * for it.
	 */
	ret = coresight_grab_device(csdev);
	if (ret)
		return ret;

	node = kzalloc(sizeof(struct coresight_node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->csdev = csdev;
	list_add(&node->link, path);

	return 0;
}

struct list_head *coresight_build_path(struct coresight_device *source,
				       struct coresight_device *sink)
{
	struct list_head *path;
	int rc;

	if (!sink)
		return ERR_PTR(-EINVAL);

	path = kzalloc(sizeof(struct list_head), GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(path);

	rc = _coresight_build_path(source, sink, path);
	if (rc) {
		kfree(path);
		return ERR_PTR(rc);
	}

	return path;
}

/**
 * coresight_release_path - release a previously built path.
 * @path:	the path to release.
 *
 * Go through all the elements of a path and 1) removed it from the list and
 * 2) free the memory allocated for each node.
 */
void coresight_release_path(struct list_head *path)
{
	struct coresight_device *csdev;
	struct coresight_node *nd, *next;

	list_for_each_entry_safe(nd, next, path, link) {
		csdev = nd->csdev;

		coresight_drop_device(csdev);
		list_del(&nd->link);
		kfree(nd);
	}

	kfree(path);
}

/* return true if the device is a suitable type for a default sink */
static inline bool coresight_is_def_sink_type(struct coresight_device *csdev)
{
	/* sink & correct subtype */
	if (((csdev->type == CORESIGHT_DEV_TYPE_SINK) ||
	     (csdev->type == CORESIGHT_DEV_TYPE_LINKSINK)) &&
	    (csdev->subtype.sink_subtype >= CORESIGHT_DEV_SUBTYPE_SINK_BUFFER))
		return true;
	return false;
}

/**
 * coresight_select_best_sink - return the best sink for use as default from
 * the two provided.
 *
 * @sink:	current best sink.
 * @depth:      search depth where current sink was found.
 * @new_sink:	new sink for comparison with current sink.
 * @new_depth:  search depth where new sink was found.
 *
 * Sinks prioritised according to coresight_dev_subtype_sink, with only
 * subtypes CORESIGHT_DEV_SUBTYPE_SINK_BUFFER or higher being used.
 *
 * Where two sinks of equal priority are found, the sink closest to the
 * source is used (smallest search depth).
 *
 * return @new_sink & update @depth if better than @sink, else return @sink.
 */
static struct coresight_device *
coresight_select_best_sink(struct coresight_device *sink, int *depth,
			   struct coresight_device *new_sink, int new_depth)
{
	bool update = false;

	if (!sink) {
		/* first found at this level */
		update = true;
	} else if (new_sink->subtype.sink_subtype >
		   sink->subtype.sink_subtype) {
		/* found better sink */
		update = true;
	} else if ((new_sink->subtype.sink_subtype ==
		    sink->subtype.sink_subtype) &&
		   (*depth > new_depth)) {
		/* found same but closer sink */
		update = true;
	}

	if (update)
		*depth = new_depth;
	return update ? new_sink : sink;
}

/**
 * coresight_find_sink - recursive function to walk trace connections from
 * source to find a suitable default sink.
 *
 * @csdev: source / current device to check.
 * @depth: [in] search depth of calling dev, [out] depth of found sink.
 *
 * This will walk the connection path from a source (ETM) till a suitable
 * sink is encountered and return that sink to the original caller.
 *
 * If current device is a plain sink return that & depth, otherwise recursively
 * call child connections looking for a sink. Select best possible using
 * coresight_select_best_sink.
 *
 * return best sink found, or NULL if not found at this node or child nodes.
 */
static struct coresight_device *
coresight_find_sink(struct coresight_device *csdev, int *depth)
{
	int i, curr_depth = *depth + 1, found_depth = 0;
	struct coresight_device *found_sink = NULL;

	if (coresight_is_def_sink_type(csdev)) {
		found_depth = curr_depth;
		found_sink = csdev;
		if (csdev->type == CORESIGHT_DEV_TYPE_SINK)
			goto return_def_sink;
		/* look past LINKSINK for something better */
	}

	/*
	 * Not a sink we want - or possible child sink may be better.
	 * recursively explore each port found on this element.
	 */
	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child_dev, *sink = NULL;
		int child_depth = curr_depth;

		child_dev = csdev->pdata->out_conns[i]->dest_dev;
		if (child_dev)
			sink = coresight_find_sink(child_dev, &child_depth);

		if (sink)
			found_sink = coresight_select_best_sink(found_sink,
								&found_depth,
								sink,
								child_depth);
	}

return_def_sink:
	/* return found sink and depth */
	if (found_sink)
		*depth = found_depth;
	return found_sink;
}

/**
 * coresight_find_default_sink: Find a sink suitable for use as a
 * default sink.
 *
 * @csdev: starting source to find a connected sink.
 *
 * Walks connections graph looking for a suitable sink to enable for the
 * supplied source. Uses CoreSight device subtypes and distance from source
 * to select the best sink.
 *
 * If a sink is found, then the default sink for this device is set and
 * will be automatically used in future.
 *
 * Used in cases where the CoreSight user (perf / sysfs) has not selected a
 * sink.
 */
struct coresight_device *
coresight_find_default_sink(struct coresight_device *csdev)
{
	int depth = 0;

	/* look for a default sink if we have not found for this device */
	if (!csdev->def_sink) {
		if (coresight_is_percpu_source(csdev))
			csdev->def_sink = per_cpu(csdev_sink, source_ops(csdev)->cpu_id(csdev));
		if (!csdev->def_sink)
			csdev->def_sink = coresight_find_sink(csdev, &depth);
	}
	return csdev->def_sink;
}

static int coresight_remove_sink_ref(struct device *dev, void *data)
{
	struct coresight_device *sink = data;
	struct coresight_device *source = to_coresight_device(dev);

	if (source->def_sink == sink)
		source->def_sink = NULL;
	return 0;
}

/**
 * coresight_clear_default_sink: Remove all default sink references to the
 * supplied sink.
 *
 * If supplied device is a sink, then check all the bus devices and clear
 * out all the references to this sink from the coresight_device def_sink
 * parameter.
 *
 * @csdev: coresight sink - remove references to this from all sources.
 */
static void coresight_clear_default_sink(struct coresight_device *csdev)
{
	if ((csdev->type == CORESIGHT_DEV_TYPE_SINK) ||
	    (csdev->type == CORESIGHT_DEV_TYPE_LINKSINK)) {
		bus_for_each_dev(&coresight_bustype, NULL, csdev,
				 coresight_remove_sink_ref);
	}
}

/** coresight_validate_source - make sure a source has the right credentials
 *  @csdev:	the device structure for a source.
 *  @function:	the function this was called from.
 *
 * Assumes the coresight_mutex is held.
 */
static int coresight_validate_source(struct coresight_device *csdev,
				     const char *function)
{
	u32 type, subtype;

	type = csdev->type;
	subtype = csdev->subtype.source_subtype;

	if (type != CORESIGHT_DEV_TYPE_SOURCE) {
		dev_err(&csdev->dev, "wrong device type in %s\n", function);
		return -EINVAL;
	}

	if (subtype != CORESIGHT_DEV_SUBTYPE_SOURCE_PROC &&
	    subtype != CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE &&
	    subtype != CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS) {
		dev_err(&csdev->dev, "wrong device subtype in %s\n", function);
		return -EINVAL;
	}

	return 0;
}

int coresight_enable(struct coresight_device *csdev)
{
	int cpu, ret = 0;
	struct coresight_device *sink;
	struct list_head *path;
	enum coresight_dev_subtype_source subtype;
	u32 hash;

	subtype = csdev->subtype.source_subtype;

	mutex_lock(&coresight_mutex);

	ret = coresight_validate_source(csdev, __func__);
	if (ret)
		goto out;

	if (csdev->enable) {
		/*
		 * There could be multiple applications driving the software
		 * source. So keep the refcount for each such user when the
		 * source is already enabled.
		 */
		if (subtype == CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE)
			atomic_inc(&csdev->refcnt);
		goto out;
	}

	sink = coresight_get_enabled_sink(csdev);
	if (!sink) {
		ret = -EINVAL;
		goto out;
	}

	path = coresight_build_path(csdev, sink);
	if (IS_ERR(path)) {
		pr_err("building path(s) failed\n");
		ret = PTR_ERR(path);
		goto out;
	}

	ret = coresight_enable_path(path, CS_MODE_SYSFS, NULL);
	if (ret)
		goto err_path;

	ret = coresight_enable_source(csdev, CS_MODE_SYSFS, NULL);
	if (ret)
		goto err_source;

	switch (subtype) {
	case CORESIGHT_DEV_SUBTYPE_SOURCE_PROC:
		/*
		 * When working from sysFS it is important to keep track
		 * of the paths that were created so that they can be
		 * undone in 'coresight_disable()'.  Since there can only
		 * be a single session per tracer (when working from sysFS)
		 * a per-cpu variable will do just fine.
		 */
		cpu = source_ops(csdev)->cpu_id(csdev);
		per_cpu(tracer_path, cpu) = path;
		break;
	case CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE:
	case CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS:
		/*
		 * Use the hash of source's device name as ID
		 * and map the ID to the pointer of the path.
		 */
		hash = hashlen_hash(hashlen_string(NULL, dev_name(&csdev->dev)));
		ret = idr_alloc_u32(&path_idr, path, &hash, hash, GFP_KERNEL);
		if (ret)
			goto err_source;
		break;
	default:
		/* We can't be here */
		break;
	}

out:
	mutex_unlock(&coresight_mutex);
	return ret;

err_source:
	coresight_disable_path(path);

err_path:
	coresight_release_path(path);
	goto out;
}
EXPORT_SYMBOL_GPL(coresight_enable);

void coresight_disable(struct coresight_device *csdev)
{
	int cpu, ret;
	struct list_head *path = NULL;
	u32 hash;

	mutex_lock(&coresight_mutex);

	ret = coresight_validate_source(csdev, __func__);
	if (ret)
		goto out;

	if (!csdev->enable || !coresight_disable_source(csdev, NULL))
		goto out;

	switch (csdev->subtype.source_subtype) {
	case CORESIGHT_DEV_SUBTYPE_SOURCE_PROC:
		cpu = source_ops(csdev)->cpu_id(csdev);
		path = per_cpu(tracer_path, cpu);
		per_cpu(tracer_path, cpu) = NULL;
		break;
	case CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE:
	case CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS:
		hash = hashlen_hash(hashlen_string(NULL, dev_name(&csdev->dev)));
		/* Find the path by the hash. */
		path = idr_find(&path_idr, hash);
		if (path == NULL) {
			pr_err("Path is not found for %s\n", dev_name(&csdev->dev));
			goto out;
		}
		idr_remove(&path_idr, hash);
		break;
	default:
		/* We can't be here */
		break;
	}

	coresight_disable_path(path);
	coresight_release_path(path);

out:
	mutex_unlock(&coresight_mutex);
}
EXPORT_SYMBOL_GPL(coresight_disable);

static ssize_t enable_sink_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct coresight_device *csdev = to_coresight_device(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", csdev->activated);
}

static ssize_t enable_sink_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	int ret;
	unsigned long val;
	struct coresight_device *csdev = to_coresight_device(dev);

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val)
		csdev->activated = true;
	else
		csdev->activated = false;

	return size;

}
static DEVICE_ATTR_RW(enable_sink);

static ssize_t enable_source_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct coresight_device *csdev = to_coresight_device(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", csdev->enable);
}

static ssize_t enable_source_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	int ret = 0;
	unsigned long val;
	struct coresight_device *csdev = to_coresight_device(dev);

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val) {
		ret = coresight_enable(csdev);
		if (ret)
			return ret;
	} else {
		coresight_disable(csdev);
	}

	return size;
}
static DEVICE_ATTR_RW(enable_source);

static struct attribute *coresight_sink_attrs[] = {
	&dev_attr_enable_sink.attr,
	NULL,
};
ATTRIBUTE_GROUPS(coresight_sink);

static struct attribute *coresight_source_attrs[] = {
	&dev_attr_enable_source.attr,
	NULL,
};
ATTRIBUTE_GROUPS(coresight_source);

static struct device_type coresight_dev_type[] = {
	{
		.name = "sink",
		.groups = coresight_sink_groups,
	},
	{
		.name = "link",
	},
	{
		.name = "linksink",
		.groups = coresight_sink_groups,
	},
	{
		.name = "source",
		.groups = coresight_source_groups,
	},
	{
		.name = "helper",
	}
};
/* Ensure the enum matches the names and groups */
static_assert(ARRAY_SIZE(coresight_dev_type) == CORESIGHT_DEV_TYPE_MAX);

static void coresight_device_release(struct device *dev)
{
	struct coresight_device *csdev = to_coresight_device(dev);

	fwnode_handle_put(csdev->dev.fwnode);
	kfree(csdev);
}

static int coresight_orphan_match(struct device *dev, void *data)
{
	int i, ret = 0;
	bool still_orphan = false;
	struct coresight_device *dst_csdev = data;
	struct coresight_device *src_csdev = to_coresight_device(dev);
	struct coresight_connection *conn;
	bool fixup_self = (src_csdev == dst_csdev);

	/* Move on to another component if no connection is orphan */
	if (!src_csdev->orphan)
		return 0;
	/*
	 * Circle through all the connections of that component.  If we find
	 * an orphan connection whose name matches @dst_csdev, link it.
	 */
	for (i = 0; i < src_csdev->pdata->nr_outconns; i++) {
		conn = src_csdev->pdata->out_conns[i];

		/* Skip the port if it's already connected. */
		if (conn->dest_dev)
			continue;

		/*
		 * If we are at the "new" device, which triggered this search,
		 * we must find the remote device from the fwnode in the
		 * connection.
		 */
		if (fixup_self)
			dst_csdev = coresight_find_csdev_by_fwnode(
				conn->dest_fwnode);

		/* Does it match this newly added device? */
		if (dst_csdev && conn->dest_fwnode == dst_csdev->dev.fwnode) {
			ret = coresight_make_links(src_csdev, conn, dst_csdev);
			if (ret)
				return ret;

			/*
			 * Install the device connection. This also indicates that
			 * the links are operational on both ends.
			 */
			conn->dest_dev = dst_csdev;
			conn->src_dev = src_csdev;

			ret = coresight_add_in_conn(conn);
			if (ret)
				return ret;
		} else {
			/* This component still has an orphan */
			still_orphan = true;
		}
	}

	src_csdev->orphan = still_orphan;

	/*
	 * Returning '0' in case we didn't encounter any error,
	 * ensures that all known component on the bus will be checked.
	 */
	return 0;
}

static int coresight_fixup_orphan_conns(struct coresight_device *csdev)
{
	return bus_for_each_dev(&coresight_bustype, NULL,
			 csdev, coresight_orphan_match);
}

/* coresight_remove_conns - Remove other device's references to this device */
static void coresight_remove_conns(struct coresight_device *csdev)
{
	int i, j;
	struct coresight_connection *conn;

	/*
	 * Remove the input connection references from the destination device
	 * for each output connection.
	 */
	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		conn = csdev->pdata->out_conns[i];
		if (!conn->dest_dev)
			continue;

		for (j = 0; j < conn->dest_dev->pdata->nr_inconns; ++j)
			if (conn->dest_dev->pdata->in_conns[j] == conn) {
				conn->dest_dev->pdata->in_conns[j] = NULL;
				break;
			}
	}

	/*
	 * For all input connections, remove references to this device.
	 * Connection objects are shared so modifying this device's input
	 * connections affects the other device's output connection.
	 */
	for (i = 0; i < csdev->pdata->nr_inconns; ++i) {
		conn = csdev->pdata->in_conns[i];
		/* Input conns array is sparse */
		if (!conn)
			continue;

		conn->src_dev->orphan = true;
		coresight_remove_links(conn->src_dev, conn);
		conn->dest_dev = NULL;
	}
}

/**
 * coresight_timeout - loop until a bit has changed to a specific register
 *			state.
 * @csa: coresight device access for the device
 * @offset: Offset of the register from the base of the device.
 * @position: the position of the bit of interest.
 * @value: the value the bit should have.
 *
 * Return: 0 as soon as the bit has taken the desired state or -EAGAIN if
 * TIMEOUT_US has elapsed, which ever happens first.
 */
int coresight_timeout(struct csdev_access *csa, u32 offset,
		      int position, int value)
{
	int i;
	u32 val;

	for (i = TIMEOUT_US; i > 0; i--) {
		val = csdev_access_read32(csa, offset);
		/* waiting on the bit to go from 0 to 1 */
		if (value) {
			if (val & BIT(position))
				return 0;
		/* waiting on the bit to go from 1 to 0 */
		} else {
			if (!(val & BIT(position)))
				return 0;
		}

		/*
		 * Delay is arbitrary - the specification doesn't say how long
		 * we are expected to wait.  Extra check required to make sure
		 * we don't wait needlessly on the last iteration.
		 */
		if (i - 1)
			udelay(1);
	}

	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(coresight_timeout);

u32 coresight_relaxed_read32(struct coresight_device *csdev, u32 offset)
{
	return csdev_access_relaxed_read32(&csdev->access, offset);
}

u32 coresight_read32(struct coresight_device *csdev, u32 offset)
{
	return csdev_access_read32(&csdev->access, offset);
}

void coresight_relaxed_write32(struct coresight_device *csdev,
			       u32 val, u32 offset)
{
	csdev_access_relaxed_write32(&csdev->access, val, offset);
}

void coresight_write32(struct coresight_device *csdev, u32 val, u32 offset)
{
	csdev_access_write32(&csdev->access, val, offset);
}

u64 coresight_relaxed_read64(struct coresight_device *csdev, u32 offset)
{
	return csdev_access_relaxed_read64(&csdev->access, offset);
}

u64 coresight_read64(struct coresight_device *csdev, u32 offset)
{
	return csdev_access_read64(&csdev->access, offset);
}

void coresight_relaxed_write64(struct coresight_device *csdev,
			       u64 val, u32 offset)
{
	csdev_access_relaxed_write64(&csdev->access, val, offset);
}

void coresight_write64(struct coresight_device *csdev, u64 val, u32 offset)
{
	csdev_access_write64(&csdev->access, val, offset);
}

/*
 * coresight_release_platform_data: Release references to the devices connected
 * to the output port of this device.
 */
void coresight_release_platform_data(struct coresight_device *csdev,
				     struct device *dev,
				     struct coresight_platform_data *pdata)
{
	int i;
	struct coresight_connection **conns = pdata->out_conns;

	for (i = 0; i < pdata->nr_outconns; i++) {
		/* If we have made the links, remove them now */
		if (csdev && conns[i]->dest_dev)
			coresight_remove_links(csdev, conns[i]);
		/*
		 * Drop the refcount and clear the handle as this device
		 * is going away
		 */
		fwnode_handle_put(conns[i]->dest_fwnode);
		conns[i]->dest_fwnode = NULL;
		devm_kfree(dev, conns[i]);
	}
	devm_kfree(dev, pdata->out_conns);
	devm_kfree(dev, pdata->in_conns);
	devm_kfree(dev, pdata);
	if (csdev)
		coresight_remove_conns_sysfs_group(csdev);
}

struct coresight_device *coresight_register(struct coresight_desc *desc)
{
	int ret;
	struct coresight_device *csdev;
	bool registered = false;

	csdev = kzalloc(sizeof(*csdev), GFP_KERNEL);
	if (!csdev) {
		ret = -ENOMEM;
		goto err_out;
	}

	csdev->pdata = desc->pdata;

	csdev->type = desc->type;
	csdev->subtype = desc->subtype;
	csdev->ops = desc->ops;
	csdev->access = desc->access;
	csdev->orphan = true;

	csdev->dev.type = &coresight_dev_type[desc->type];
	csdev->dev.groups = desc->groups;
	csdev->dev.parent = desc->dev;
	csdev->dev.release = coresight_device_release;
	csdev->dev.bus = &coresight_bustype;
	/*
	 * Hold the reference to our parent device. This will be
	 * dropped only in coresight_device_release().
	 */
	csdev->dev.fwnode = fwnode_handle_get(dev_fwnode(desc->dev));
	dev_set_name(&csdev->dev, "%s", desc->name);

	/*
	 * Make sure the device registration and the connection fixup
	 * are synchronised, so that we don't see uninitialised devices
	 * on the coresight bus while trying to resolve the connections.
	 */
	mutex_lock(&coresight_mutex);

	ret = device_register(&csdev->dev);
	if (ret) {
		put_device(&csdev->dev);
		/*
		 * All resources are free'd explicitly via
		 * coresight_device_release(), triggered from put_device().
		 */
		goto out_unlock;
	}

	if (csdev->type == CORESIGHT_DEV_TYPE_SINK ||
	    csdev->type == CORESIGHT_DEV_TYPE_LINKSINK) {
		ret = etm_perf_add_symlink_sink(csdev);

		if (ret) {
			device_unregister(&csdev->dev);
			/*
			 * As with the above, all resources are free'd
			 * explicitly via coresight_device_release() triggered
			 * from put_device(), which is in turn called from
			 * function device_unregister().
			 */
			goto out_unlock;
		}
	}
	/* Device is now registered */
	registered = true;

	ret = coresight_create_conns_sysfs_group(csdev);
	if (!ret)
		ret = coresight_fixup_orphan_conns(csdev);

out_unlock:
	mutex_unlock(&coresight_mutex);
	/* Success */
	if (!ret) {
		if (cti_assoc_ops && cti_assoc_ops->add)
			cti_assoc_ops->add(csdev);
		return csdev;
	}

	/* Unregister the device if needed */
	if (registered) {
		coresight_unregister(csdev);
		return ERR_PTR(ret);
	}

err_out:
	/* Cleanup the connection information */
	coresight_release_platform_data(NULL, desc->dev, desc->pdata);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(coresight_register);

void coresight_unregister(struct coresight_device *csdev)
{
	etm_perf_del_symlink_sink(csdev);
	/* Remove references of that device in the topology */
	if (cti_assoc_ops && cti_assoc_ops->remove)
		cti_assoc_ops->remove(csdev);
	coresight_remove_conns(csdev);
	coresight_clear_default_sink(csdev);
	coresight_release_platform_data(csdev, csdev->dev.parent, csdev->pdata);
	device_unregister(&csdev->dev);
}
EXPORT_SYMBOL_GPL(coresight_unregister);


/*
 * coresight_search_device_idx - Search the fwnode handle of a device
 * in the given dev_idx list. Must be called with the coresight_mutex held.
 *
 * Returns the index of the entry, when found. Otherwise, -ENOENT.
 */
static inline int coresight_search_device_idx(struct coresight_dev_list *dict,
					      struct fwnode_handle *fwnode)
{
	int i;

	for (i = 0; i < dict->nr_idx; i++)
		if (dict->fwnode_list[i] == fwnode)
			return i;
	return -ENOENT;
}

static bool coresight_compare_type(enum coresight_dev_type type_a,
				   union coresight_dev_subtype subtype_a,
				   enum coresight_dev_type type_b,
				   union coresight_dev_subtype subtype_b)
{
	if (type_a != type_b)
		return false;

	switch (type_a) {
	case CORESIGHT_DEV_TYPE_SINK:
		return subtype_a.sink_subtype == subtype_b.sink_subtype;
	case CORESIGHT_DEV_TYPE_LINK:
		return subtype_a.link_subtype == subtype_b.link_subtype;
	case CORESIGHT_DEV_TYPE_LINKSINK:
		return subtype_a.link_subtype == subtype_b.link_subtype &&
		       subtype_a.sink_subtype == subtype_b.sink_subtype;
	case CORESIGHT_DEV_TYPE_SOURCE:
		return subtype_a.source_subtype == subtype_b.source_subtype;
	case CORESIGHT_DEV_TYPE_HELPER:
		return subtype_a.helper_subtype == subtype_b.helper_subtype;
	default:
		return false;
	}
}

struct coresight_device *
coresight_find_input_type(struct coresight_platform_data *pdata,
			  enum coresight_dev_type type,
			  union coresight_dev_subtype subtype)
{
	int i;
	struct coresight_connection *conn;

	for (i = 0; i < pdata->nr_inconns; ++i) {
		conn = pdata->in_conns[i];
		if (conn &&
		    coresight_compare_type(type, subtype, conn->src_dev->type,
					   conn->src_dev->subtype))
			return conn->src_dev;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(coresight_find_input_type);

struct coresight_device *
coresight_find_output_type(struct coresight_platform_data *pdata,
			   enum coresight_dev_type type,
			   union coresight_dev_subtype subtype)
{
	int i;
	struct coresight_connection *conn;

	for (i = 0; i < pdata->nr_outconns; ++i) {
		conn = pdata->out_conns[i];
		if (conn->dest_dev &&
		    coresight_compare_type(type, subtype, conn->dest_dev->type,
					   conn->dest_dev->subtype))
			return conn->dest_dev;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(coresight_find_output_type);

bool coresight_loses_context_with_cpu(struct device *dev)
{
	return fwnode_property_present(dev_fwnode(dev),
				       "arm,coresight-loses-context-with-cpu");
}
EXPORT_SYMBOL_GPL(coresight_loses_context_with_cpu);

/*
 * coresight_alloc_device_name - Get an index for a given device in the
 * device index list specific to a driver. An index is allocated for a
 * device and is tracked with the fwnode_handle to prevent allocating
 * duplicate indices for the same device (e.g, if we defer probing of
 * a device due to dependencies), in case the index is requested again.
 */
char *coresight_alloc_device_name(struct coresight_dev_list *dict,
				  struct device *dev)
{
	int idx;
	char *name = NULL;
	struct fwnode_handle **list;

	mutex_lock(&coresight_mutex);

	idx = coresight_search_device_idx(dict, dev_fwnode(dev));
	if (idx < 0) {
		/* Make space for the new entry */
		idx = dict->nr_idx;
		list = krealloc_array(dict->fwnode_list,
				      idx + 1, sizeof(*dict->fwnode_list),
				      GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(list)) {
			idx = -ENOMEM;
			goto done;
		}

		list[idx] = dev_fwnode(dev);
		dict->fwnode_list = list;
		dict->nr_idx = idx + 1;
	}

	name = devm_kasprintf(dev, GFP_KERNEL, "%s%d", dict->pfx, idx);
done:
	mutex_unlock(&coresight_mutex);
	return name;
}
EXPORT_SYMBOL_GPL(coresight_alloc_device_name);

struct bus_type coresight_bustype = {
	.name	= "coresight",
};

static int __init coresight_init(void)
{
	int ret;

	ret = bus_register(&coresight_bustype);
	if (ret)
		return ret;

	ret = etm_perf_init();
	if (ret)
		goto exit_bus_unregister;

	/* initialise the coresight syscfg API */
	ret = cscfg_init();
	if (!ret)
		return 0;

	etm_perf_exit();
exit_bus_unregister:
	bus_unregister(&coresight_bustype);
	return ret;
}

static void __exit coresight_exit(void)
{
	cscfg_exit();
	etm_perf_exit();
	bus_unregister(&coresight_bustype);
}

module_init(coresight_init);
module_exit(coresight_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pratik Patel <pratikp@codeaurora.org>");
MODULE_AUTHOR("Mathieu Poirier <mathieu.poirier@linaro.org>");
MODULE_DESCRIPTION("Arm CoreSight tracer driver");
