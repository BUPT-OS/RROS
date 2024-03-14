/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DRM_GEM_DMA_HELPER_H__
#define __DRM_GEM_DMA_HELPER_H__

#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>

struct drm_mode_create_dumb;

/**
 * struct drm_gem_dma_object - GEM object backed by DMA memory allocations
 * @base: base GEM object
 * @dma_addr: DMA address of the backing memory
 * @sgt: scatter/gather table for imported PRIME buffers. The table can have
 *       more than one entry but they are guaranteed to have contiguous
 *       DMA addresses.
 * @vaddr: kernel virtual address of the backing memory
 * @map_noncoherent: if true, the GEM object is backed by non-coherent memory
 */
struct drm_gem_dma_object {
	struct drm_gem_object base;
	dma_addr_t dma_addr;
	struct sg_table *sgt;

	/* For objects with DMA memory allocated by GEM DMA */
	void *vaddr;

	bool map_noncoherent;
};

#define to_drm_gem_dma_obj(gem_obj) \
	container_of(gem_obj, struct drm_gem_dma_object, base)

struct drm_gem_dma_object *drm_gem_dma_create(struct drm_device *drm,
					      size_t size);
void drm_gem_dma_free(struct drm_gem_dma_object *dma_obj);
void drm_gem_dma_print_info(const struct drm_gem_dma_object *dma_obj,
			    struct drm_printer *p, unsigned int indent);
struct sg_table *drm_gem_dma_get_sg_table(struct drm_gem_dma_object *dma_obj);
int drm_gem_dma_vmap(struct drm_gem_dma_object *dma_obj,
		     struct iosys_map *map);
int drm_gem_dma_mmap(struct drm_gem_dma_object *dma_obj, struct vm_area_struct *vma);

extern const struct vm_operations_struct drm_gem_dma_vm_ops;

/*
 * GEM object functions
 */

/**
 * drm_gem_dma_object_free - GEM object function for drm_gem_dma_free()
 * @obj: GEM object to free
 *
 * This function wraps drm_gem_dma_free_object(). Drivers that employ the DMA helpers
 * should use it as their &drm_gem_object_funcs.free handler.
 */
static inline void drm_gem_dma_object_free(struct drm_gem_object *obj)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	drm_gem_dma_free(dma_obj);
}

/**
 * drm_gem_dma_object_print_info() - Print &drm_gem_dma_object info for debugfs
 * @p: DRM printer
 * @indent: Tab indentation level
 * @obj: GEM object
 *
 * This function wraps drm_gem_dma_print_info(). Drivers that employ the DMA helpers
 * should use this function as their &drm_gem_object_funcs.print_info handler.
 */
static inline void drm_gem_dma_object_print_info(struct drm_printer *p, unsigned int indent,
						 const struct drm_gem_object *obj)
{
	const struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	drm_gem_dma_print_info(dma_obj, p, indent);
}

/**
 * drm_gem_dma_object_get_sg_table - GEM object function for drm_gem_dma_get_sg_table()
 * @obj: GEM object
 *
 * This function wraps drm_gem_dma_get_sg_table(). Drivers that employ the DMA helpers should
 * use it as their &drm_gem_object_funcs.get_sg_table handler.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or NULL on failure.
 */
static inline struct sg_table *drm_gem_dma_object_get_sg_table(struct drm_gem_object *obj)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	return drm_gem_dma_get_sg_table(dma_obj);
}

/*
 * drm_gem_dma_object_vmap - GEM object function for drm_gem_dma_vmap()
 * @obj: GEM object
 * @map: Returns the kernel virtual address of the DMA GEM object's backing store.
 *
 * This function wraps drm_gem_dma_vmap(). Drivers that employ the DMA helpers should
 * use it as their &drm_gem_object_funcs.vmap handler.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
static inline int drm_gem_dma_object_vmap(struct drm_gem_object *obj,
					  struct iosys_map *map)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	return drm_gem_dma_vmap(dma_obj, map);
}

/**
 * drm_gem_dma_object_mmap - GEM object function for drm_gem_dma_mmap()
 * @obj: GEM object
 * @vma: VMA for the area to be mapped
 *
 * This function wraps drm_gem_dma_mmap(). Drivers that employ the dma helpers should
 * use it as their &drm_gem_object_funcs.mmap handler.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
static inline int drm_gem_dma_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	return drm_gem_dma_mmap(dma_obj, vma);
}

/*
 * Driver ops
 */

/* create memory region for DRM framebuffer */
int drm_gem_dma_dumb_create_internal(struct drm_file *file_priv,
				     struct drm_device *drm,
				     struct drm_mode_create_dumb *args);

/* create memory region for DRM framebuffer */
int drm_gem_dma_dumb_create(struct drm_file *file_priv,
			    struct drm_device *drm,
			    struct drm_mode_create_dumb *args);

struct drm_gem_object *
drm_gem_dma_prime_import_sg_table(struct drm_device *dev,
				  struct dma_buf_attachment *attach,
				  struct sg_table *sgt);

/**
 * DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE - DMA GEM driver operations
 * @dumb_create_func: callback function for .dumb_create
 *
 * This macro provides a shortcut for setting the default GEM operations in the
 * &drm_driver structure.
 *
 * This macro is a variant of DRM_GEM_DMA_DRIVER_OPS for drivers that
 * override the default implementation of &struct rm_driver.dumb_create. Use
 * DRM_GEM_DMA_DRIVER_OPS if possible. Drivers that require a virtual address
 * on imported buffers should use
 * DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE() instead.
 */
#define DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(dumb_create_func) \
	.dumb_create		   = (dumb_create_func), \
	.gem_prime_import_sg_table = drm_gem_dma_prime_import_sg_table

/**
 * DRM_GEM_DMA_DRIVER_OPS - DMA GEM driver operations
 *
 * This macro provides a shortcut for setting the default GEM operations in the
 * &drm_driver structure.
 *
 * Drivers that come with their own implementation of
 * &struct drm_driver.dumb_create should use
 * DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE() instead. Use
 * DRM_GEM_DMA_DRIVER_OPS if possible. Drivers that require a virtual address
 * on imported buffers should use DRM_GEM_DMA_DRIVER_OPS_VMAP instead.
 */
#define DRM_GEM_DMA_DRIVER_OPS \
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(drm_gem_dma_dumb_create)

/**
 * DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE - DMA GEM driver operations
 *                                                ensuring a virtual address
 *                                                on the buffer
 * @dumb_create_func: callback function for .dumb_create
 *
 * This macro provides a shortcut for setting the default GEM operations in the
 * &drm_driver structure for drivers that need the virtual address also on
 * imported buffers.
 *
 * This macro is a variant of DRM_GEM_DMA_DRIVER_OPS_VMAP for drivers that
 * override the default implementation of &struct drm_driver.dumb_create. Use
 * DRM_GEM_DMA_DRIVER_OPS_VMAP if possible. Drivers that do not require a
 * virtual address on imported buffers should use
 * DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE() instead.
 */
#define DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE(dumb_create_func) \
	.dumb_create		   = (dumb_create_func), \
	.gem_prime_import_sg_table = drm_gem_dma_prime_import_sg_table_vmap

/**
 * DRM_GEM_DMA_DRIVER_OPS_VMAP - DMA GEM driver operations ensuring a virtual
 *                               address on the buffer
 *
 * This macro provides a shortcut for setting the default GEM operations in the
 * &drm_driver structure for drivers that need the virtual address also on
 * imported buffers.
 *
 * Drivers that come with their own implementation of
 * &struct drm_driver.dumb_create should use
 * DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE() instead. Use
 * DRM_GEM_DMA_DRIVER_OPS_VMAP if possible. Drivers that do not require a
 * virtual address on imported buffers should use DRM_GEM_DMA_DRIVER_OPS
 * instead.
 */
#define DRM_GEM_DMA_DRIVER_OPS_VMAP \
	DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE(drm_gem_dma_dumb_create)

struct drm_gem_object *
drm_gem_dma_prime_import_sg_table_vmap(struct drm_device *drm,
				       struct dma_buf_attachment *attach,
				       struct sg_table *sgt);

/*
 * File ops
 */

#ifndef CONFIG_MMU
unsigned long drm_gem_dma_get_unmapped_area(struct file *filp,
					    unsigned long addr,
					    unsigned long len,
					    unsigned long pgoff,
					    unsigned long flags);
#define DRM_GEM_DMA_UNMAPPED_AREA_FOPS \
	.get_unmapped_area	= drm_gem_dma_get_unmapped_area,
#else
#define DRM_GEM_DMA_UNMAPPED_AREA_FOPS
#endif

/**
 * DEFINE_DRM_GEM_DMA_FOPS() - macro to generate file operations for DMA drivers
 * @name: name for the generated structure
 *
 * This macro autogenerates a suitable &struct file_operations for DMA based
 * drivers, which can be assigned to &drm_driver.fops. Note that this structure
 * cannot be shared between drivers, because it contains a reference to the
 * current module using THIS_MODULE.
 *
 * Note that the declaration is already marked as static - if you need a
 * non-static version of this you're probably doing it wrong and will break the
 * THIS_MODULE reference by accident.
 */
#define DEFINE_DRM_GEM_DMA_FOPS(name) \
	static const struct file_operations name = {\
		.owner		= THIS_MODULE,\
		.open		= drm_open,\
		.release	= drm_release,\
		.unlocked_ioctl	= drm_ioctl,\
		.compat_ioctl	= drm_compat_ioctl,\
		.poll		= drm_poll,\
		.read		= drm_read,\
		.llseek		= noop_llseek,\
		.mmap		= drm_gem_mmap,\
		DRM_GEM_DMA_UNMAPPED_AREA_FOPS \
	}

#endif /* __DRM_GEM_DMA_HELPER_H__ */
