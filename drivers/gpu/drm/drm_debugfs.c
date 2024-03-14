/*
 * Created: Sun Dec 21 13:08:50 2008 by bgamari@gmail.com
 *
 * Copyright 2008 Ben Gamari <bgamari@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_atomic.h>
#include <drm/drm_auth.h>
#include <drm/drm_bridge.h>
#include <drm/drm_client.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/drm_gpuva_mgr.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

#if defined(CONFIG_DEBUG_FS)

/***************************************************
 * Initialization, etc.
 **************************************************/

static int drm_name_info(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct drm_master *master;

	mutex_lock(&dev->master_mutex);
	master = dev->master;
	seq_printf(m, "%s", dev->driver->name);
	if (dev->dev)
		seq_printf(m, " dev=%s", dev_name(dev->dev));
	if (master && master->unique)
		seq_printf(m, " master=%s", master->unique);
	if (dev->unique)
		seq_printf(m, " unique=%s", dev->unique);
	seq_printf(m, "\n");
	mutex_unlock(&dev->master_mutex);

	return 0;
}

static int drm_clients_info(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct drm_file *priv;
	kuid_t uid;

	seq_printf(m,
		   "%20s %5s %3s master a %5s %10s\n",
		   "command",
		   "tgid",
		   "dev",
		   "uid",
		   "magic");

	/* dev->filelist is sorted youngest first, but we want to present
	 * oldest first (i.e. kernel, servers, clients), so walk backwardss.
	 */
	mutex_lock(&dev->filelist_mutex);
	list_for_each_entry_reverse(priv, &dev->filelist, lhead) {
		struct task_struct *task;
		bool is_current_master = drm_is_current_master(priv);

		rcu_read_lock(); /* locks pid_task()->comm */
		task = pid_task(priv->pid, PIDTYPE_TGID);
		uid = task ? __task_cred(task)->euid : GLOBAL_ROOT_UID;
		seq_printf(m, "%20s %5d %3d   %c    %c %5d %10u\n",
			   task ? task->comm : "<unknown>",
			   pid_vnr(priv->pid),
			   priv->minor->index,
			   is_current_master ? 'y' : 'n',
			   priv->authenticated ? 'y' : 'n',
			   from_kuid_munged(seq_user_ns(m), uid),
			   priv->magic);
		rcu_read_unlock();
	}
	mutex_unlock(&dev->filelist_mutex);
	return 0;
}

static int drm_gem_one_name_info(int id, void *ptr, void *data)
{
	struct drm_gem_object *obj = ptr;
	struct seq_file *m = data;

	seq_printf(m, "%6d %8zd %7d %8d\n",
		   obj->name, obj->size,
		   obj->handle_count,
		   kref_read(&obj->refcount));
	return 0;
}

static int drm_gem_name_info(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;

	seq_printf(m, "  name     size handles refcount\n");

	mutex_lock(&dev->object_name_lock);
	idr_for_each(&dev->object_name_idr, drm_gem_one_name_info, m);
	mutex_unlock(&dev->object_name_lock);

	return 0;
}

static const struct drm_debugfs_info drm_debugfs_list[] = {
	{"name", drm_name_info, 0},
	{"clients", drm_clients_info, 0},
	{"gem_names", drm_gem_name_info, DRIVER_GEM},
};
#define DRM_DEBUGFS_ENTRIES ARRAY_SIZE(drm_debugfs_list)


static int drm_debugfs_open(struct inode *inode, struct file *file)
{
	struct drm_info_node *node = inode->i_private;

	return single_open(file, node->info_ent->show, node);
}

static int drm_debugfs_entry_open(struct inode *inode, struct file *file)
{
	struct drm_debugfs_entry *entry = inode->i_private;
	struct drm_debugfs_info *node = &entry->file;

	return single_open(file, node->show, entry);
}

static const struct file_operations drm_debugfs_entry_fops = {
	.owner = THIS_MODULE,
	.open = drm_debugfs_entry_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations drm_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = drm_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * drm_debugfs_gpuva_info - dump the given DRM GPU VA space
 * @m: pointer to the &seq_file to write
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 *
 * Dumps the GPU VA mappings of a given DRM GPU VA manager.
 *
 * For each DRM GPU VA space drivers should call this function from their
 * &drm_info_list's show callback.
 *
 * Returns: 0 on success, -ENODEV if the &mgr is not initialized
 */
int drm_debugfs_gpuva_info(struct seq_file *m,
			   struct drm_gpuva_manager *mgr)
{
	struct drm_gpuva *va, *kva = &mgr->kernel_alloc_node;

	if (!mgr->name)
		return -ENODEV;

	seq_printf(m, "DRM GPU VA space (%s) [0x%016llx;0x%016llx]\n",
		   mgr->name, mgr->mm_start, mgr->mm_start + mgr->mm_range);
	seq_printf(m, "Kernel reserved node [0x%016llx;0x%016llx]\n",
		   kva->va.addr, kva->va.addr + kva->va.range);
	seq_puts(m, "\n");
	seq_puts(m, " VAs | start              | range              | end                | object             | object offset\n");
	seq_puts(m, "-------------------------------------------------------------------------------------------------------------\n");
	drm_gpuva_for_each_va(va, mgr) {
		if (unlikely(va == kva))
			continue;

		seq_printf(m, "     | 0x%016llx | 0x%016llx | 0x%016llx | 0x%016llx | 0x%016llx\n",
			   va->va.addr, va->va.range, va->va.addr + va->va.range,
			   (u64)(uintptr_t)va->gem.obj, va->gem.offset);
	}

	return 0;
}
EXPORT_SYMBOL(drm_debugfs_gpuva_info);

/**
 * drm_debugfs_create_files - Initialize a given set of debugfs files for DRM
 * 			minor
 * @files: The array of files to create
 * @count: The number of files given
 * @root: DRI debugfs dir entry.
 * @minor: device minor number
 *
 * Create a given set of debugfs files represented by an array of
 * &struct drm_info_list in the given root directory. These files will be removed
 * automatically on drm_debugfs_cleanup().
 */
void drm_debugfs_create_files(const struct drm_info_list *files, int count,
			      struct dentry *root, struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_info_node *tmp;
	int i;

	for (i = 0; i < count; i++) {
		u32 features = files[i].driver_features;

		if (features && !drm_core_check_all_features(dev, features))
			continue;

		tmp = kmalloc(sizeof(struct drm_info_node), GFP_KERNEL);
		if (tmp == NULL)
			continue;

		tmp->minor = minor;
		tmp->dent = debugfs_create_file(files[i].name,
						0444, root, tmp,
						&drm_debugfs_fops);
		tmp->info_ent = &files[i];

		mutex_lock(&minor->debugfs_lock);
		list_add(&tmp->list, &minor->debugfs_list);
		mutex_unlock(&minor->debugfs_lock);
	}
}
EXPORT_SYMBOL(drm_debugfs_create_files);

int drm_debugfs_init(struct drm_minor *minor, int minor_id,
		     struct dentry *root)
{
	struct drm_device *dev = minor->dev;
	struct drm_debugfs_entry *entry, *tmp;
	char name[64];

	INIT_LIST_HEAD(&minor->debugfs_list);
	mutex_init(&minor->debugfs_lock);
	sprintf(name, "%d", minor_id);
	minor->debugfs_root = debugfs_create_dir(name, root);

	drm_debugfs_add_files(minor->dev, drm_debugfs_list, DRM_DEBUGFS_ENTRIES);

	if (drm_drv_uses_atomic_modeset(dev)) {
		drm_atomic_debugfs_init(minor);
		drm_bridge_debugfs_init(minor);
	}

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		drm_framebuffer_debugfs_init(minor);

		drm_client_debugfs_init(minor);
	}

	if (dev->driver->debugfs_init)
		dev->driver->debugfs_init(minor);

	list_for_each_entry_safe(entry, tmp, &dev->debugfs_list, list) {
		debugfs_create_file(entry->file.name, 0444,
				    minor->debugfs_root, entry, &drm_debugfs_entry_fops);
		list_del(&entry->list);
	}

	return 0;
}

void drm_debugfs_late_register(struct drm_device *dev)
{
	struct drm_minor *minor = dev->primary;
	struct drm_debugfs_entry *entry, *tmp;

	if (!minor)
		return;

	list_for_each_entry_safe(entry, tmp, &dev->debugfs_list, list) {
		debugfs_create_file(entry->file.name, 0444,
				    minor->debugfs_root, entry, &drm_debugfs_entry_fops);
		list_del(&entry->list);
	}
}

int drm_debugfs_remove_files(const struct drm_info_list *files, int count,
			     struct drm_minor *minor)
{
	struct list_head *pos, *q;
	struct drm_info_node *tmp;
	int i;

	mutex_lock(&minor->debugfs_lock);
	for (i = 0; i < count; i++) {
		list_for_each_safe(pos, q, &minor->debugfs_list) {
			tmp = list_entry(pos, struct drm_info_node, list);
			if (tmp->info_ent == &files[i]) {
				debugfs_remove(tmp->dent);
				list_del(pos);
				kfree(tmp);
			}
		}
	}
	mutex_unlock(&minor->debugfs_lock);
	return 0;
}
EXPORT_SYMBOL(drm_debugfs_remove_files);

static void drm_debugfs_remove_all_files(struct drm_minor *minor)
{
	struct drm_info_node *node, *tmp;

	mutex_lock(&minor->debugfs_lock);
	list_for_each_entry_safe(node, tmp, &minor->debugfs_list, list) {
		debugfs_remove(node->dent);
		list_del(&node->list);
		kfree(node);
	}
	mutex_unlock(&minor->debugfs_lock);
}

void drm_debugfs_cleanup(struct drm_minor *minor)
{
	if (!minor->debugfs_root)
		return;

	drm_debugfs_remove_all_files(minor);

	debugfs_remove_recursive(minor->debugfs_root);
	minor->debugfs_root = NULL;
}

/**
 * drm_debugfs_add_file - Add a given file to the DRM device debugfs file list
 * @dev: drm device for the ioctl
 * @name: debugfs file name
 * @show: show callback
 * @data: driver-private data, should not be device-specific
 *
 * Add a given file entry to the DRM device debugfs file list to be created on
 * drm_debugfs_init.
 */
void drm_debugfs_add_file(struct drm_device *dev, const char *name,
			  int (*show)(struct seq_file*, void*), void *data)
{
	struct drm_debugfs_entry *entry = drmm_kzalloc(dev, sizeof(*entry), GFP_KERNEL);

	if (!entry)
		return;

	entry->file.name = name;
	entry->file.show = show;
	entry->file.data = data;
	entry->dev = dev;

	mutex_lock(&dev->debugfs_mutex);
	list_add(&entry->list, &dev->debugfs_list);
	mutex_unlock(&dev->debugfs_mutex);
}
EXPORT_SYMBOL(drm_debugfs_add_file);

/**
 * drm_debugfs_add_files - Add an array of files to the DRM device debugfs file list
 * @dev: drm device for the ioctl
 * @files: The array of files to create
 * @count: The number of files given
 *
 * Add a given set of debugfs files represented by an array of
 * &struct drm_debugfs_info in the DRM device debugfs file list.
 */
void drm_debugfs_add_files(struct drm_device *dev, const struct drm_debugfs_info *files, int count)
{
	int i;

	for (i = 0; i < count; i++)
		drm_debugfs_add_file(dev, files[i].name, files[i].show, files[i].data);
}
EXPORT_SYMBOL(drm_debugfs_add_files);

static int connector_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;

	seq_printf(m, "%s\n", drm_get_connector_force_name(connector->force));

	return 0;
}

static int connector_open(struct inode *inode, struct file *file)
{
	struct drm_connector *dev = inode->i_private;

	return single_open(file, connector_show, dev);
}

static ssize_t connector_write(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct drm_connector *connector = m->private;
	char buf[12];

	if (len > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	if (sysfs_streq(buf, "on"))
		connector->force = DRM_FORCE_ON;
	else if (sysfs_streq(buf, "digital"))
		connector->force = DRM_FORCE_ON_DIGITAL;
	else if (sysfs_streq(buf, "off"))
		connector->force = DRM_FORCE_OFF;
	else if (sysfs_streq(buf, "unspecified"))
		connector->force = DRM_FORCE_UNSPECIFIED;
	else
		return -EINVAL;

	return len;
}

static int edid_show(struct seq_file *m, void *data)
{
	return drm_edid_override_show(m->private, m);
}

static int edid_open(struct inode *inode, struct file *file)
{
	struct drm_connector *dev = inode->i_private;

	return single_open(file, edid_show, dev);
}

static ssize_t edid_write(struct file *file, const char __user *ubuf,
			  size_t len, loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct drm_connector *connector = m->private;
	char *buf;
	int ret;

	buf = memdup_user(ubuf, len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (len == 5 && !strncmp(buf, "reset", 5))
		ret = drm_edid_override_reset(connector);
	else
		ret = drm_edid_override_set(connector, buf, len);

	kfree(buf);

	return ret ? ret : len;
}

/*
 * Returns the min and max vrr vfreq through the connector's debugfs file.
 * Example usage: cat /sys/kernel/debug/dri/0/DP-1/vrr_range
 */
static int vrr_range_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;

	if (connector->status != connector_status_connected)
		return -ENODEV;

	seq_printf(m, "Min: %u\n", connector->display_info.monitor_range.min_vfreq);
	seq_printf(m, "Max: %u\n", connector->display_info.monitor_range.max_vfreq);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vrr_range);

/*
 * Returns Connector's max supported bpc through debugfs file.
 * Example usage: cat /sys/kernel/debug/dri/0/DP-1/output_bpc
 */
static int output_bpc_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;

	if (connector->status != connector_status_connected)
		return -ENODEV;

	seq_printf(m, "Maximum: %u\n", connector->display_info.bpc);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(output_bpc);

static const struct file_operations drm_edid_fops = {
	.owner = THIS_MODULE,
	.open = edid_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = edid_write
};


static const struct file_operations drm_connector_fops = {
	.owner = THIS_MODULE,
	.open = connector_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = connector_write
};

void drm_debugfs_connector_add(struct drm_connector *connector)
{
	struct drm_minor *minor = connector->dev->primary;
	struct dentry *root;

	if (!minor->debugfs_root)
		return;

	root = debugfs_create_dir(connector->name, minor->debugfs_root);
	connector->debugfs_entry = root;

	/* force */
	debugfs_create_file("force", 0644, root, connector,
			    &drm_connector_fops);

	/* edid */
	debugfs_create_file("edid_override", 0644, root, connector,
			    &drm_edid_fops);

	/* vrr range */
	debugfs_create_file("vrr_range", 0444, root, connector,
			    &vrr_range_fops);

	/* max bpc */
	debugfs_create_file("output_bpc", 0444, root, connector,
			    &output_bpc_fops);

	if (connector->funcs->debugfs_init)
		connector->funcs->debugfs_init(connector, root);
}

void drm_debugfs_connector_remove(struct drm_connector *connector)
{
	if (!connector->debugfs_entry)
		return;

	debugfs_remove_recursive(connector->debugfs_entry);

	connector->debugfs_entry = NULL;
}

void drm_debugfs_crtc_add(struct drm_crtc *crtc)
{
	struct drm_minor *minor = crtc->dev->primary;
	struct dentry *root;
	char *name;

	name = kasprintf(GFP_KERNEL, "crtc-%d", crtc->index);
	if (!name)
		return;

	root = debugfs_create_dir(name, minor->debugfs_root);
	kfree(name);

	crtc->debugfs_entry = root;

	drm_debugfs_crtc_crc_add(crtc);
}

void drm_debugfs_crtc_remove(struct drm_crtc *crtc)
{
	debugfs_remove_recursive(crtc->debugfs_entry);
	crtc->debugfs_entry = NULL;
}

#endif /* CONFIG_DEBUG_FS */
