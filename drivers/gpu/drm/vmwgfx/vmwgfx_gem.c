/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2021-2023 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "vmwgfx_bo.h"
#include "vmwgfx_drv.h"

#include "drm/drm_prime.h"
#include "drm/drm_gem_ttm_helper.h"

static void vmw_gem_object_free(struct drm_gem_object *gobj)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(gobj);
	if (bo)
		ttm_bo_put(bo);
}

static int vmw_gem_object_open(struct drm_gem_object *obj,
			       struct drm_file *file_priv)
{
	return 0;
}

static void vmw_gem_object_close(struct drm_gem_object *obj,
				 struct drm_file *file_priv)
{
}

static int vmw_gem_pin_private(struct drm_gem_object *obj, bool do_pin)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(obj);
	struct vmw_bo *vbo = to_vmw_bo(obj);
	int ret;

	ret = ttm_bo_reserve(bo, false, false, NULL);
	if (unlikely(ret != 0))
		goto err;

	vmw_bo_pin_reserved(vbo, do_pin);

	ttm_bo_unreserve(bo);

err:
	return ret;
}


static int vmw_gem_object_pin(struct drm_gem_object *obj)
{
	return vmw_gem_pin_private(obj, true);
}

static void vmw_gem_object_unpin(struct drm_gem_object *obj)
{
	vmw_gem_pin_private(obj, false);
}

static struct sg_table *vmw_gem_object_get_sg_table(struct drm_gem_object *obj)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(obj);
	struct vmw_ttm_tt *vmw_tt =
		container_of(bo->ttm, struct vmw_ttm_tt, dma_ttm);

	if (vmw_tt->vsgt.sgt)
		return vmw_tt->vsgt.sgt;

	return drm_prime_pages_to_sg(obj->dev, vmw_tt->dma_ttm.pages, vmw_tt->dma_ttm.num_pages);
}

static const struct vm_operations_struct vmw_vm_ops = {
	.pfn_mkwrite = vmw_bo_vm_mkwrite,
	.page_mkwrite = vmw_bo_vm_mkwrite,
	.fault = vmw_bo_vm_fault,
	.open = ttm_bo_vm_open,
	.close = ttm_bo_vm_close,
};

static const struct drm_gem_object_funcs vmw_gem_object_funcs = {
	.free = vmw_gem_object_free,
	.open = vmw_gem_object_open,
	.close = vmw_gem_object_close,
	.print_info = drm_gem_ttm_print_info,
	.pin = vmw_gem_object_pin,
	.unpin = vmw_gem_object_unpin,
	.get_sg_table = vmw_gem_object_get_sg_table,
	.vmap = drm_gem_ttm_vmap,
	.vunmap = drm_gem_ttm_vunmap,
	.mmap = drm_gem_ttm_mmap,
	.vm_ops = &vmw_vm_ops,
};

int vmw_gem_object_create_with_handle(struct vmw_private *dev_priv,
				      struct drm_file *filp,
				      uint32_t size,
				      uint32_t *handle,
				      struct vmw_bo **p_vbo)
{
	int ret;
	struct vmw_bo_params params = {
		.domain = (dev_priv->has_mob) ? VMW_BO_DOMAIN_SYS : VMW_BO_DOMAIN_VRAM,
		.busy_domain = VMW_BO_DOMAIN_SYS,
		.bo_type = ttm_bo_type_device,
		.size = size,
		.pin = false
	};

	ret = vmw_bo_create(dev_priv, &params, p_vbo);
	if (ret != 0)
		goto out_no_bo;

	(*p_vbo)->tbo.base.funcs = &vmw_gem_object_funcs;

	ret = drm_gem_handle_create(filp, &(*p_vbo)->tbo.base, handle);
out_no_bo:
	return ret;
}


int vmw_gem_object_create_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	union drm_vmw_alloc_dmabuf_arg *arg =
	    (union drm_vmw_alloc_dmabuf_arg *)data;
	struct drm_vmw_alloc_dmabuf_req *req = &arg->req;
	struct drm_vmw_dmabuf_rep *rep = &arg->rep;
	struct vmw_bo *vbo;
	uint32_t handle;
	int ret;

	ret = vmw_gem_object_create_with_handle(dev_priv, filp,
						req->size, &handle, &vbo);
	if (ret)
		goto out_no_bo;

	rep->handle = handle;
	rep->map_handle = drm_vma_node_offset_addr(&vbo->tbo.base.vma_node);
	rep->cur_gmr_id = handle;
	rep->cur_gmr_offset = 0;
	/* drop reference from allocate - handle holds it now */
	drm_gem_object_put(&vbo->tbo.base);
out_no_bo:
	return ret;
}

#if defined(CONFIG_DEBUG_FS)

static void vmw_bo_print_info(int id, struct vmw_bo *bo, struct seq_file *m)
{
	const char *placement;
	const char *type;

	switch (bo->tbo.resource->mem_type) {
	case TTM_PL_SYSTEM:
		placement = " CPU";
		break;
	case VMW_PL_GMR:
		placement = " GMR";
		break;
	case VMW_PL_MOB:
		placement = " MOB";
		break;
	case VMW_PL_SYSTEM:
		placement = "VCPU";
		break;
	case TTM_PL_VRAM:
		placement = "VRAM";
		break;
	default:
		placement = "None";
		break;
	}

	switch (bo->tbo.type) {
	case ttm_bo_type_device:
		type = "device";
		break;
	case ttm_bo_type_kernel:
		type = "kernel";
		break;
	case ttm_bo_type_sg:
		type = "sg    ";
		break;
	default:
		type = "none  ";
		break;
	}

	seq_printf(m, "\t\t0x%08x: %12zu bytes %s, type = %s",
		   id, bo->tbo.base.size, placement, type);
	seq_printf(m, ", priority = %u, pin_count = %u, GEM refs = %d, TTM refs = %d",
		   bo->tbo.priority,
		   bo->tbo.pin_count,
		   kref_read(&bo->tbo.base.refcount),
		   kref_read(&bo->tbo.kref));
	seq_puts(m, "\n");
}

static int vmw_debugfs_gem_info_show(struct seq_file *m, void *unused)
{
	struct vmw_private *vdev = (struct vmw_private *)m->private;
	struct drm_device *dev = &vdev->drm;
	struct drm_file *file;
	int r;

	r = mutex_lock_interruptible(&dev->filelist_mutex);
	if (r)
		return r;

	list_for_each_entry(file, &dev->filelist, lhead) {
		struct task_struct *task;
		struct drm_gem_object *gobj;
		int id;

		/*
		 * Although we have a valid reference on file->pid, that does
		 * not guarantee that the task_struct who called get_pid() is
		 * still alive (e.g. get_pid(current) => fork() => exit()).
		 * Therefore, we need to protect this ->comm access using RCU.
		 */
		rcu_read_lock();
		task = pid_task(file->pid, PIDTYPE_TGID);
		seq_printf(m, "pid %8d command %s:\n", pid_nr(file->pid),
			   task ? task->comm : "<unknown>");
		rcu_read_unlock();

		spin_lock(&file->table_lock);
		idr_for_each_entry(&file->object_idr, gobj, id) {
			struct vmw_bo *bo = to_vmw_bo(gobj);

			vmw_bo_print_info(id, bo, m);
		}
		spin_unlock(&file->table_lock);
	}

	mutex_unlock(&dev->filelist_mutex);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(vmw_debugfs_gem_info);

#endif

void vmw_debugfs_gem_init(struct vmw_private *vdev)
{
#if defined(CONFIG_DEBUG_FS)
	struct drm_minor *minor = vdev->drm.primary;
	struct dentry *root = minor->debugfs_root;

	debugfs_create_file("vmwgfx_gem_info", 0444, root, vdev,
			    &vmw_debugfs_gem_info_fops);
#endif
}
