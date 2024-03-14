// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include "vmwgfx_kms.h"

#include "vmwgfx_bo.h"
#include "vmw_surface_cache.h"

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_rect.h>
#include <drm/drm_sysfs.h>

void vmw_du_cleanup(struct vmw_display_unit *du)
{
	struct vmw_private *dev_priv = vmw_priv(du->primary.dev);
	drm_plane_cleanup(&du->primary);
	if (vmw_cmd_supported(dev_priv))
		drm_plane_cleanup(&du->cursor.base);

	drm_connector_unregister(&du->connector);
	drm_crtc_cleanup(&du->crtc);
	drm_encoder_cleanup(&du->encoder);
	drm_connector_cleanup(&du->connector);
}

/*
 * Display Unit Cursor functions
 */

static int vmw_du_cursor_plane_unmap_cm(struct vmw_plane_state *vps);
static void vmw_cursor_update_mob(struct vmw_private *dev_priv,
				  struct vmw_plane_state *vps,
				  u32 *image, u32 width, u32 height,
				  u32 hotspotX, u32 hotspotY);

struct vmw_svga_fifo_cmd_define_cursor {
	u32 cmd;
	SVGAFifoCmdDefineAlphaCursor cursor;
};

/**
 * vmw_send_define_cursor_cmd - queue a define cursor command
 * @dev_priv: the private driver struct
 * @image: buffer which holds the cursor image
 * @width: width of the mouse cursor image
 * @height: height of the mouse cursor image
 * @hotspotX: the horizontal position of mouse hotspot
 * @hotspotY: the vertical position of mouse hotspot
 */
static void vmw_send_define_cursor_cmd(struct vmw_private *dev_priv,
				       u32 *image, u32 width, u32 height,
				       u32 hotspotX, u32 hotspotY)
{
	struct vmw_svga_fifo_cmd_define_cursor *cmd;
	const u32 image_size = width * height * sizeof(*image);
	const u32 cmd_size = sizeof(*cmd) + image_size;

	/* Try to reserve fifocmd space and swallow any failures;
	   such reservations cannot be left unconsumed for long
	   under the risk of clogging other fifocmd users, so
	   we treat reservations separtely from the way we treat
	   other fallible KMS-atomic resources at prepare_fb */
	cmd = VMW_CMD_RESERVE(dev_priv, cmd_size);

	if (unlikely(!cmd))
		return;

	memset(cmd, 0, sizeof(*cmd));

	memcpy(&cmd[1], image, image_size);

	cmd->cmd = SVGA_CMD_DEFINE_ALPHA_CURSOR;
	cmd->cursor.id = 0;
	cmd->cursor.width = width;
	cmd->cursor.height = height;
	cmd->cursor.hotspotX = hotspotX;
	cmd->cursor.hotspotY = hotspotY;

	vmw_cmd_commit_flush(dev_priv, cmd_size);
}

/**
 * vmw_cursor_update_image - update the cursor image on the provided plane
 * @dev_priv: the private driver struct
 * @vps: the plane state of the cursor plane
 * @image: buffer which holds the cursor image
 * @width: width of the mouse cursor image
 * @height: height of the mouse cursor image
 * @hotspotX: the horizontal position of mouse hotspot
 * @hotspotY: the vertical position of mouse hotspot
 */
static void vmw_cursor_update_image(struct vmw_private *dev_priv,
				    struct vmw_plane_state *vps,
				    u32 *image, u32 width, u32 height,
				    u32 hotspotX, u32 hotspotY)
{
	if (vps->cursor.bo)
		vmw_cursor_update_mob(dev_priv, vps, image,
				      vps->base.crtc_w, vps->base.crtc_h,
				      hotspotX, hotspotY);

	else
		vmw_send_define_cursor_cmd(dev_priv, image, width, height,
					   hotspotX, hotspotY);
}


/**
 * vmw_cursor_update_mob - Update cursor vis CursorMob mechanism
 *
 * Called from inside vmw_du_cursor_plane_atomic_update to actually
 * make the cursor-image live.
 *
 * @dev_priv: device to work with
 * @vps: the plane state of the cursor plane
 * @image: cursor source data to fill the MOB with
 * @width: source data width
 * @height: source data height
 * @hotspotX: cursor hotspot x
 * @hotspotY: cursor hotspot Y
 */
static void vmw_cursor_update_mob(struct vmw_private *dev_priv,
				  struct vmw_plane_state *vps,
				  u32 *image, u32 width, u32 height,
				  u32 hotspotX, u32 hotspotY)
{
	SVGAGBCursorHeader *header;
	SVGAGBAlphaCursorHeader *alpha_header;
	const u32 image_size = width * height * sizeof(*image);

	header = vmw_bo_map_and_cache(vps->cursor.bo);
	alpha_header = &header->header.alphaHeader;

	memset(header, 0, sizeof(*header));

	header->type = SVGA_ALPHA_CURSOR;
	header->sizeInBytes = image_size;

	alpha_header->hotspotX = hotspotX;
	alpha_header->hotspotY = hotspotY;
	alpha_header->width = width;
	alpha_header->height = height;

	memcpy(header + 1, image, image_size);
	vmw_write(dev_priv, SVGA_REG_CURSOR_MOBID,
		  vps->cursor.bo->tbo.resource->start);
}


static u32 vmw_du_cursor_mob_size(u32 w, u32 h)
{
	return w * h * sizeof(u32) + sizeof(SVGAGBCursorHeader);
}

/**
 * vmw_du_cursor_plane_acquire_image -- Acquire the image data
 * @vps: cursor plane state
 */
static u32 *vmw_du_cursor_plane_acquire_image(struct vmw_plane_state *vps)
{
	bool is_iomem;
	if (vps->surf) {
		if (vps->surf_mapped)
			return vmw_bo_map_and_cache(vps->surf->res.guest_memory_bo);
		return vps->surf->snooper.image;
	} else if (vps->bo)
		return ttm_kmap_obj_virtual(&vps->bo->map, &is_iomem);
	return NULL;
}

static bool vmw_du_cursor_plane_has_changed(struct vmw_plane_state *old_vps,
					    struct vmw_plane_state *new_vps)
{
	void *old_image;
	void *new_image;
	u32 size;
	bool changed;

	if (old_vps->base.crtc_w != new_vps->base.crtc_w ||
	    old_vps->base.crtc_h != new_vps->base.crtc_h)
	    return true;

	if (old_vps->cursor.hotspot_x != new_vps->cursor.hotspot_x ||
	    old_vps->cursor.hotspot_y != new_vps->cursor.hotspot_y)
	    return true;

	size = new_vps->base.crtc_w * new_vps->base.crtc_h * sizeof(u32);

	old_image = vmw_du_cursor_plane_acquire_image(old_vps);
	new_image = vmw_du_cursor_plane_acquire_image(new_vps);

	changed = false;
	if (old_image && new_image)
		changed = memcmp(old_image, new_image, size) != 0;

	return changed;
}

static void vmw_du_destroy_cursor_mob(struct vmw_bo **vbo)
{
	if (!(*vbo))
		return;

	ttm_bo_unpin(&(*vbo)->tbo);
	vmw_bo_unreference(vbo);
}

static void vmw_du_put_cursor_mob(struct vmw_cursor_plane *vcp,
				  struct vmw_plane_state *vps)
{
	u32 i;

	if (!vps->cursor.bo)
		return;

	vmw_du_cursor_plane_unmap_cm(vps);

	/* Look for a free slot to return this mob to the cache. */
	for (i = 0; i < ARRAY_SIZE(vcp->cursor_mobs); i++) {
		if (!vcp->cursor_mobs[i]) {
			vcp->cursor_mobs[i] = vps->cursor.bo;
			vps->cursor.bo = NULL;
			return;
		}
	}

	/* Cache is full: See if this mob is bigger than an existing mob. */
	for (i = 0; i < ARRAY_SIZE(vcp->cursor_mobs); i++) {
		if (vcp->cursor_mobs[i]->tbo.base.size <
		    vps->cursor.bo->tbo.base.size) {
			vmw_du_destroy_cursor_mob(&vcp->cursor_mobs[i]);
			vcp->cursor_mobs[i] = vps->cursor.bo;
			vps->cursor.bo = NULL;
			return;
		}
	}

	/* Destroy it if it's not worth caching. */
	vmw_du_destroy_cursor_mob(&vps->cursor.bo);
}

static int vmw_du_get_cursor_mob(struct vmw_cursor_plane *vcp,
				 struct vmw_plane_state *vps)
{
	struct vmw_private *dev_priv = vcp->base.dev->dev_private;
	u32 size = vmw_du_cursor_mob_size(vps->base.crtc_w, vps->base.crtc_h);
	u32 i;
	u32 cursor_max_dim, mob_max_size;
	int ret;

	if (!dev_priv->has_mob ||
	    (dev_priv->capabilities2 & SVGA_CAP2_CURSOR_MOB) == 0)
		return -EINVAL;

	mob_max_size = vmw_read(dev_priv, SVGA_REG_MOB_MAX_SIZE);
	cursor_max_dim = vmw_read(dev_priv, SVGA_REG_CURSOR_MAX_DIMENSION);

	if (size > mob_max_size || vps->base.crtc_w > cursor_max_dim ||
	    vps->base.crtc_h > cursor_max_dim)
		return -EINVAL;

	if (vps->cursor.bo) {
		if (vps->cursor.bo->tbo.base.size >= size)
			return 0;
		vmw_du_put_cursor_mob(vcp, vps);
	}

	/* Look for an unused mob in the cache. */
	for (i = 0; i < ARRAY_SIZE(vcp->cursor_mobs); i++) {
		if (vcp->cursor_mobs[i] &&
		    vcp->cursor_mobs[i]->tbo.base.size >= size) {
			vps->cursor.bo = vcp->cursor_mobs[i];
			vcp->cursor_mobs[i] = NULL;
			return 0;
		}
	}
	/* Create a new mob if we can't find an existing one. */
	ret = vmw_bo_create_and_populate(dev_priv, size,
					 VMW_BO_DOMAIN_MOB,
					 &vps->cursor.bo);

	if (ret != 0)
		return ret;

	/* Fence the mob creation so we are guarateed to have the mob */
	ret = ttm_bo_reserve(&vps->cursor.bo->tbo, false, false, NULL);
	if (ret != 0)
		goto teardown;

	vmw_bo_fence_single(&vps->cursor.bo->tbo, NULL);
	ttm_bo_unreserve(&vps->cursor.bo->tbo);
	return 0;

teardown:
	vmw_du_destroy_cursor_mob(&vps->cursor.bo);
	return ret;
}


static void vmw_cursor_update_position(struct vmw_private *dev_priv,
				       bool show, int x, int y)
{
	const uint32_t svga_cursor_on = show ? SVGA_CURSOR_ON_SHOW
					     : SVGA_CURSOR_ON_HIDE;
	uint32_t count;

	spin_lock(&dev_priv->cursor_lock);
	if (dev_priv->capabilities2 & SVGA_CAP2_EXTRA_REGS) {
		vmw_write(dev_priv, SVGA_REG_CURSOR4_X, x);
		vmw_write(dev_priv, SVGA_REG_CURSOR4_Y, y);
		vmw_write(dev_priv, SVGA_REG_CURSOR4_SCREEN_ID, SVGA3D_INVALID_ID);
		vmw_write(dev_priv, SVGA_REG_CURSOR4_ON, svga_cursor_on);
		vmw_write(dev_priv, SVGA_REG_CURSOR4_SUBMIT, 1);
	} else if (vmw_is_cursor_bypass3_enabled(dev_priv)) {
		vmw_fifo_mem_write(dev_priv, SVGA_FIFO_CURSOR_ON, svga_cursor_on);
		vmw_fifo_mem_write(dev_priv, SVGA_FIFO_CURSOR_X, x);
		vmw_fifo_mem_write(dev_priv, SVGA_FIFO_CURSOR_Y, y);
		count = vmw_fifo_mem_read(dev_priv, SVGA_FIFO_CURSOR_COUNT);
		vmw_fifo_mem_write(dev_priv, SVGA_FIFO_CURSOR_COUNT, ++count);
	} else {
		vmw_write(dev_priv, SVGA_REG_CURSOR_X, x);
		vmw_write(dev_priv, SVGA_REG_CURSOR_Y, y);
		vmw_write(dev_priv, SVGA_REG_CURSOR_ON, svga_cursor_on);
	}
	spin_unlock(&dev_priv->cursor_lock);
}

void vmw_kms_cursor_snoop(struct vmw_surface *srf,
			  struct ttm_object_file *tfile,
			  struct ttm_buffer_object *bo,
			  SVGA3dCmdHeader *header)
{
	struct ttm_bo_kmap_obj map;
	unsigned long kmap_offset;
	unsigned long kmap_num;
	SVGA3dCopyBox *box;
	unsigned box_count;
	void *virtual;
	bool is_iomem;
	struct vmw_dma_cmd {
		SVGA3dCmdHeader header;
		SVGA3dCmdSurfaceDMA dma;
	} *cmd;
	int i, ret;
	const struct SVGA3dSurfaceDesc *desc =
		vmw_surface_get_desc(VMW_CURSOR_SNOOP_FORMAT);
	const u32 image_pitch = VMW_CURSOR_SNOOP_WIDTH * desc->pitchBytesPerBlock;

	cmd = container_of(header, struct vmw_dma_cmd, header);

	/* No snooper installed, nothing to copy */
	if (!srf->snooper.image)
		return;

	if (cmd->dma.host.face != 0 || cmd->dma.host.mipmap != 0) {
		DRM_ERROR("face and mipmap for cursors should never != 0\n");
		return;
	}

	if (cmd->header.size < 64) {
		DRM_ERROR("at least one full copy box must be given\n");
		return;
	}

	box = (SVGA3dCopyBox *)&cmd[1];
	box_count = (cmd->header.size - sizeof(SVGA3dCmdSurfaceDMA)) /
			sizeof(SVGA3dCopyBox);

	if (cmd->dma.guest.ptr.offset % PAGE_SIZE ||
	    box->x != 0    || box->y != 0    || box->z != 0    ||
	    box->srcx != 0 || box->srcy != 0 || box->srcz != 0 ||
	    box->d != 1    || box_count != 1 ||
	    box->w > VMW_CURSOR_SNOOP_WIDTH || box->h > VMW_CURSOR_SNOOP_HEIGHT) {
		/* TODO handle none page aligned offsets */
		/* TODO handle more dst & src != 0 */
		/* TODO handle more then one copy */
		DRM_ERROR("Can't snoop dma request for cursor!\n");
		DRM_ERROR("(%u, %u, %u) (%u, %u, %u) (%ux%ux%u) %u %u\n",
			  box->srcx, box->srcy, box->srcz,
			  box->x, box->y, box->z,
			  box->w, box->h, box->d, box_count,
			  cmd->dma.guest.ptr.offset);
		return;
	}

	kmap_offset = cmd->dma.guest.ptr.offset >> PAGE_SHIFT;
	kmap_num = (VMW_CURSOR_SNOOP_HEIGHT*image_pitch) >> PAGE_SHIFT;

	ret = ttm_bo_reserve(bo, true, false, NULL);
	if (unlikely(ret != 0)) {
		DRM_ERROR("reserve failed\n");
		return;
	}

	ret = ttm_bo_kmap(bo, kmap_offset, kmap_num, &map);
	if (unlikely(ret != 0))
		goto err_unreserve;

	virtual = ttm_kmap_obj_virtual(&map, &is_iomem);

	if (box->w == VMW_CURSOR_SNOOP_WIDTH && cmd->dma.guest.pitch == image_pitch) {
		memcpy(srf->snooper.image, virtual,
		       VMW_CURSOR_SNOOP_HEIGHT*image_pitch);
	} else {
		/* Image is unsigned pointer. */
		for (i = 0; i < box->h; i++)
			memcpy(srf->snooper.image + i * image_pitch,
			       virtual + i * cmd->dma.guest.pitch,
			       box->w * desc->pitchBytesPerBlock);
	}

	srf->snooper.age++;

	ttm_bo_kunmap(&map);
err_unreserve:
	ttm_bo_unreserve(bo);
}

/**
 * vmw_kms_legacy_hotspot_clear - Clear legacy hotspots
 *
 * @dev_priv: Pointer to the device private struct.
 *
 * Clears all legacy hotspots.
 */
void vmw_kms_legacy_hotspot_clear(struct vmw_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	struct vmw_display_unit *du;
	struct drm_crtc *crtc;

	drm_modeset_lock_all(dev);
	drm_for_each_crtc(crtc, dev) {
		du = vmw_crtc_to_du(crtc);

		du->hotspot_x = 0;
		du->hotspot_y = 0;
	}
	drm_modeset_unlock_all(dev);
}

void vmw_kms_cursor_post_execbuf(struct vmw_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	struct vmw_display_unit *du;
	struct drm_crtc *crtc;

	mutex_lock(&dev->mode_config.mutex);

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		du = vmw_crtc_to_du(crtc);
		if (!du->cursor_surface ||
		    du->cursor_age == du->cursor_surface->snooper.age ||
		    !du->cursor_surface->snooper.image)
			continue;

		du->cursor_age = du->cursor_surface->snooper.age;
		vmw_send_define_cursor_cmd(dev_priv,
					   du->cursor_surface->snooper.image,
					   VMW_CURSOR_SNOOP_WIDTH,
					   VMW_CURSOR_SNOOP_HEIGHT,
					   du->hotspot_x + du->core_hotspot_x,
					   du->hotspot_y + du->core_hotspot_y);
	}

	mutex_unlock(&dev->mode_config.mutex);
}


void vmw_du_cursor_plane_destroy(struct drm_plane *plane)
{
	struct vmw_cursor_plane *vcp = vmw_plane_to_vcp(plane);
	u32 i;

	vmw_cursor_update_position(plane->dev->dev_private, false, 0, 0);

	for (i = 0; i < ARRAY_SIZE(vcp->cursor_mobs); i++)
		vmw_du_destroy_cursor_mob(&vcp->cursor_mobs[i]);

	drm_plane_cleanup(plane);
}


void vmw_du_primary_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);

	/* Planes are static in our case so we don't free it */
}


/**
 * vmw_du_plane_unpin_surf - unpins resource associated with a framebuffer surface
 *
 * @vps: plane state associated with the display surface
 * @unreference: true if we also want to unreference the display.
 */
void vmw_du_plane_unpin_surf(struct vmw_plane_state *vps,
			     bool unreference)
{
	if (vps->surf) {
		if (vps->pinned) {
			vmw_resource_unpin(&vps->surf->res);
			vps->pinned--;
		}

		if (unreference) {
			if (vps->pinned)
				DRM_ERROR("Surface still pinned\n");
			vmw_surface_unreference(&vps->surf);
		}
	}
}


/**
 * vmw_du_plane_cleanup_fb - Unpins the plane surface
 *
 * @plane:  display plane
 * @old_state: Contains the FB to clean up
 *
 * Unpins the framebuffer surface
 *
 * Returns 0 on success
 */
void
vmw_du_plane_cleanup_fb(struct drm_plane *plane,
			struct drm_plane_state *old_state)
{
	struct vmw_plane_state *vps = vmw_plane_state_to_vps(old_state);

	vmw_du_plane_unpin_surf(vps, false);
}


/**
 * vmw_du_cursor_plane_map_cm - Maps the cursor mobs.
 *
 * @vps: plane_state
 *
 * Returns 0 on success
 */

static int
vmw_du_cursor_plane_map_cm(struct vmw_plane_state *vps)
{
	int ret;
	u32 size = vmw_du_cursor_mob_size(vps->base.crtc_w, vps->base.crtc_h);
	struct ttm_buffer_object *bo;

	if (!vps->cursor.bo)
		return -EINVAL;

	bo = &vps->cursor.bo->tbo;

	if (bo->base.size < size)
		return -EINVAL;

	if (vps->cursor.bo->map.virtual)
		return 0;

	ret = ttm_bo_reserve(bo, false, false, NULL);
	if (unlikely(ret != 0))
		return -ENOMEM;

	vmw_bo_map_and_cache(vps->cursor.bo);

	ttm_bo_unreserve(bo);

	if (unlikely(ret != 0))
		return -ENOMEM;

	return 0;
}


/**
 * vmw_du_cursor_plane_unmap_cm - Unmaps the cursor mobs.
 *
 * @vps: state of the cursor plane
 *
 * Returns 0 on success
 */

static int
vmw_du_cursor_plane_unmap_cm(struct vmw_plane_state *vps)
{
	int ret = 0;
	struct vmw_bo *vbo = vps->cursor.bo;

	if (!vbo || !vbo->map.virtual)
		return 0;

	ret = ttm_bo_reserve(&vbo->tbo, true, false, NULL);
	if (likely(ret == 0)) {
		vmw_bo_unmap(vbo);
		ttm_bo_unreserve(&vbo->tbo);
	}

	return ret;
}


/**
 * vmw_du_cursor_plane_cleanup_fb - Unpins the plane surface
 *
 * @plane: cursor plane
 * @old_state: contains the state to clean up
 *
 * Unmaps all cursor bo mappings and unpins the cursor surface
 *
 * Returns 0 on success
 */
void
vmw_du_cursor_plane_cleanup_fb(struct drm_plane *plane,
			       struct drm_plane_state *old_state)
{
	struct vmw_cursor_plane *vcp = vmw_plane_to_vcp(plane);
	struct vmw_plane_state *vps = vmw_plane_state_to_vps(old_state);
	bool is_iomem;

	if (vps->surf_mapped) {
		vmw_bo_unmap(vps->surf->res.guest_memory_bo);
		vps->surf_mapped = false;
	}

	if (vps->bo && ttm_kmap_obj_virtual(&vps->bo->map, &is_iomem)) {
		const int ret = ttm_bo_reserve(&vps->bo->tbo, true, false, NULL);

		if (likely(ret == 0)) {
			ttm_bo_kunmap(&vps->bo->map);
			ttm_bo_unreserve(&vps->bo->tbo);
		}
	}

	vmw_du_cursor_plane_unmap_cm(vps);
	vmw_du_put_cursor_mob(vcp, vps);

	vmw_du_plane_unpin_surf(vps, false);

	if (vps->surf) {
		vmw_surface_unreference(&vps->surf);
		vps->surf = NULL;
	}

	if (vps->bo) {
		vmw_bo_unreference(&vps->bo);
		vps->bo = NULL;
	}
}


/**
 * vmw_du_cursor_plane_prepare_fb - Readies the cursor by referencing it
 *
 * @plane:  display plane
 * @new_state: info on the new plane state, including the FB
 *
 * Returns 0 on success
 */
int
vmw_du_cursor_plane_prepare_fb(struct drm_plane *plane,
			       struct drm_plane_state *new_state)
{
	struct drm_framebuffer *fb = new_state->fb;
	struct vmw_cursor_plane *vcp = vmw_plane_to_vcp(plane);
	struct vmw_plane_state *vps = vmw_plane_state_to_vps(new_state);
	int ret = 0;

	if (vps->surf) {
		vmw_surface_unreference(&vps->surf);
		vps->surf = NULL;
	}

	if (vps->bo) {
		vmw_bo_unreference(&vps->bo);
		vps->bo = NULL;
	}

	if (fb) {
		if (vmw_framebuffer_to_vfb(fb)->bo) {
			vps->bo = vmw_framebuffer_to_vfbd(fb)->buffer;
			vmw_bo_reference(vps->bo);
		} else {
			vps->surf = vmw_framebuffer_to_vfbs(fb)->surface;
			vmw_surface_reference(vps->surf);
		}
	}

	if (!vps->surf && vps->bo) {
		const u32 size = new_state->crtc_w * new_state->crtc_h * sizeof(u32);

		/*
		 * Not using vmw_bo_map_and_cache() helper here as we need to
		 * reserve the ttm_buffer_object first which
		 * vmw_bo_map_and_cache() omits.
		 */
		ret = ttm_bo_reserve(&vps->bo->tbo, true, false, NULL);

		if (unlikely(ret != 0))
			return -ENOMEM;

		ret = ttm_bo_kmap(&vps->bo->tbo, 0, PFN_UP(size), &vps->bo->map);

		ttm_bo_unreserve(&vps->bo->tbo);

		if (unlikely(ret != 0))
			return -ENOMEM;
	} else if (vps->surf && !vps->bo && vps->surf->res.guest_memory_bo) {

		WARN_ON(vps->surf->snooper.image);
		ret = ttm_bo_reserve(&vps->surf->res.guest_memory_bo->tbo, true, false,
				     NULL);
		if (unlikely(ret != 0))
			return -ENOMEM;
		vmw_bo_map_and_cache(vps->surf->res.guest_memory_bo);
		ttm_bo_unreserve(&vps->surf->res.guest_memory_bo->tbo);
		vps->surf_mapped = true;
	}

	if (vps->surf || vps->bo) {
		vmw_du_get_cursor_mob(vcp, vps);
		vmw_du_cursor_plane_map_cm(vps);
	}

	return 0;
}


void
vmw_du_cursor_plane_atomic_update(struct drm_plane *plane,
				  struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state,
									   plane);
	struct drm_crtc *crtc = new_state->crtc ?: old_state->crtc;
	struct vmw_private *dev_priv = vmw_priv(crtc->dev);
	struct vmw_display_unit *du = vmw_crtc_to_du(crtc);
	struct vmw_plane_state *vps = vmw_plane_state_to_vps(new_state);
	struct vmw_plane_state *old_vps = vmw_plane_state_to_vps(old_state);
	s32 hotspot_x, hotspot_y;

	hotspot_x = du->hotspot_x;
	hotspot_y = du->hotspot_y;

	if (new_state->fb) {
		hotspot_x += new_state->fb->hot_x;
		hotspot_y += new_state->fb->hot_y;
	}

	du->cursor_surface = vps->surf;
	du->cursor_bo = vps->bo;

	if (!vps->surf && !vps->bo) {
		vmw_cursor_update_position(dev_priv, false, 0, 0);
		return;
	}

	vps->cursor.hotspot_x = hotspot_x;
	vps->cursor.hotspot_y = hotspot_y;

	if (vps->surf) {
		du->cursor_age = du->cursor_surface->snooper.age;
	}

	if (!vmw_du_cursor_plane_has_changed(old_vps, vps)) {
		/*
		 * If it hasn't changed, avoid making the device do extra
		 * work by keeping the old cursor active.
		 */
		struct vmw_cursor_plane_state tmp = old_vps->cursor;
		old_vps->cursor = vps->cursor;
		vps->cursor = tmp;
	} else {
		void *image = vmw_du_cursor_plane_acquire_image(vps);
		if (image)
			vmw_cursor_update_image(dev_priv, vps, image,
						new_state->crtc_w,
						new_state->crtc_h,
						hotspot_x, hotspot_y);
	}

	du->cursor_x = new_state->crtc_x + du->set_gui_x;
	du->cursor_y = new_state->crtc_y + du->set_gui_y;

	vmw_cursor_update_position(dev_priv, true,
				   du->cursor_x + hotspot_x,
				   du->cursor_y + hotspot_y);

	du->core_hotspot_x = hotspot_x - du->hotspot_x;
	du->core_hotspot_y = hotspot_y - du->hotspot_y;
}


/**
 * vmw_du_primary_plane_atomic_check - check if the new state is okay
 *
 * @plane: display plane
 * @state: info on the new plane state, including the FB
 *
 * Check if the new state is settable given the current state.  Other
 * than what the atomic helper checks, we care about crtc fitting
 * the FB and maintaining one active framebuffer.
 *
 * Returns 0 on success
 */
int vmw_du_primary_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct drm_crtc_state *crtc_state = NULL;
	struct drm_framebuffer *new_fb = new_state->fb;
	int ret;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   new_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, true);

	if (!ret && new_fb) {
		struct drm_crtc *crtc = new_state->crtc;
		struct vmw_display_unit *du = vmw_crtc_to_du(crtc);

		vmw_connector_state_to_vcs(du->connector.state);
	}


	return ret;
}


/**
 * vmw_du_cursor_plane_atomic_check - check if the new state is okay
 *
 * @plane: cursor plane
 * @state: info on the new plane state
 *
 * This is a chance to fail if the new cursor state does not fit
 * our requirements.
 *
 * Returns 0 on success
 */
int vmw_du_cursor_plane_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	int ret = 0;
	struct drm_crtc_state *crtc_state = NULL;
	struct vmw_surface *surface = NULL;
	struct drm_framebuffer *fb = new_state->fb;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(new_state->state,
							   new_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret)
		return ret;

	/* Turning off */
	if (!fb)
		return 0;

	/* A lot of the code assumes this */
	if (new_state->crtc_w != 64 || new_state->crtc_h != 64) {
		DRM_ERROR("Invalid cursor dimensions (%d, %d)\n",
			  new_state->crtc_w, new_state->crtc_h);
		return -EINVAL;
	}

	if (!vmw_framebuffer_to_vfb(fb)->bo) {
		surface = vmw_framebuffer_to_vfbs(fb)->surface;

		WARN_ON(!surface);

		if (!surface ||
		    (!surface->snooper.image && !surface->res.guest_memory_bo)) {
			DRM_ERROR("surface not suitable for cursor\n");
			return -EINVAL;
		}
	}

	return 0;
}


int vmw_du_crtc_atomic_check(struct drm_crtc *crtc,
			     struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state,
									 crtc);
	struct vmw_display_unit *du = vmw_crtc_to_du(new_state->crtc);
	int connector_mask = drm_connector_mask(&du->connector);
	bool has_primary = new_state->plane_mask &
			   drm_plane_mask(crtc->primary);

	/* We always want to have an active plane with an active CRTC */
	if (has_primary != new_state->enable)
		return -EINVAL;


	if (new_state->connector_mask != connector_mask &&
	    new_state->connector_mask != 0) {
		DRM_ERROR("Invalid connectors configuration\n");
		return -EINVAL;
	}

	/*
	 * Our virtual device does not have a dot clock, so use the logical
	 * clock value as the dot clock.
	 */
	if (new_state->mode.crtc_clock == 0)
		new_state->adjusted_mode.crtc_clock = new_state->mode.clock;

	return 0;
}


void vmw_du_crtc_atomic_begin(struct drm_crtc *crtc,
			      struct drm_atomic_state *state)
{
}


void vmw_du_crtc_atomic_flush(struct drm_crtc *crtc,
			      struct drm_atomic_state *state)
{
}


/**
 * vmw_du_crtc_duplicate_state - duplicate crtc state
 * @crtc: DRM crtc
 *
 * Allocates and returns a copy of the crtc state (both common and
 * vmw-specific) for the specified crtc.
 *
 * Returns: The newly allocated crtc state, or NULL on failure.
 */
struct drm_crtc_state *
vmw_du_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct drm_crtc_state *state;
	struct vmw_crtc_state *vcs;

	if (WARN_ON(!crtc->state))
		return NULL;

	vcs = kmemdup(crtc->state, sizeof(*vcs), GFP_KERNEL);

	if (!vcs)
		return NULL;

	state = &vcs->base;

	__drm_atomic_helper_crtc_duplicate_state(crtc, state);

	return state;
}


/**
 * vmw_du_crtc_reset - creates a blank vmw crtc state
 * @crtc: DRM crtc
 *
 * Resets the atomic state for @crtc by freeing the state pointer (which
 * might be NULL, e.g. at driver load time) and allocating a new empty state
 * object.
 */
void vmw_du_crtc_reset(struct drm_crtc *crtc)
{
	struct vmw_crtc_state *vcs;


	if (crtc->state) {
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

		kfree(vmw_crtc_state_to_vcs(crtc->state));
	}

	vcs = kzalloc(sizeof(*vcs), GFP_KERNEL);

	if (!vcs) {
		DRM_ERROR("Cannot allocate vmw_crtc_state\n");
		return;
	}

	__drm_atomic_helper_crtc_reset(crtc, &vcs->base);
}


/**
 * vmw_du_crtc_destroy_state - destroy crtc state
 * @crtc: DRM crtc
 * @state: state object to destroy
 *
 * Destroys the crtc state (both common and vmw-specific) for the
 * specified plane.
 */
void
vmw_du_crtc_destroy_state(struct drm_crtc *crtc,
			  struct drm_crtc_state *state)
{
	drm_atomic_helper_crtc_destroy_state(crtc, state);
}


/**
 * vmw_du_plane_duplicate_state - duplicate plane state
 * @plane: drm plane
 *
 * Allocates and returns a copy of the plane state (both common and
 * vmw-specific) for the specified plane.
 *
 * Returns: The newly allocated plane state, or NULL on failure.
 */
struct drm_plane_state *
vmw_du_plane_duplicate_state(struct drm_plane *plane)
{
	struct drm_plane_state *state;
	struct vmw_plane_state *vps;

	vps = kmemdup(plane->state, sizeof(*vps), GFP_KERNEL);

	if (!vps)
		return NULL;

	vps->pinned = 0;
	vps->cpp = 0;

	memset(&vps->cursor, 0, sizeof(vps->cursor));

	/* Each ref counted resource needs to be acquired again */
	if (vps->surf)
		(void) vmw_surface_reference(vps->surf);

	if (vps->bo)
		(void) vmw_bo_reference(vps->bo);

	state = &vps->base;

	__drm_atomic_helper_plane_duplicate_state(plane, state);

	return state;
}


/**
 * vmw_du_plane_reset - creates a blank vmw plane state
 * @plane: drm plane
 *
 * Resets the atomic state for @plane by freeing the state pointer (which might
 * be NULL, e.g. at driver load time) and allocating a new empty state object.
 */
void vmw_du_plane_reset(struct drm_plane *plane)
{
	struct vmw_plane_state *vps;

	if (plane->state)
		vmw_du_plane_destroy_state(plane, plane->state);

	vps = kzalloc(sizeof(*vps), GFP_KERNEL);

	if (!vps) {
		DRM_ERROR("Cannot allocate vmw_plane_state\n");
		return;
	}

	__drm_atomic_helper_plane_reset(plane, &vps->base);
}


/**
 * vmw_du_plane_destroy_state - destroy plane state
 * @plane: DRM plane
 * @state: state object to destroy
 *
 * Destroys the plane state (both common and vmw-specific) for the
 * specified plane.
 */
void
vmw_du_plane_destroy_state(struct drm_plane *plane,
			   struct drm_plane_state *state)
{
	struct vmw_plane_state *vps = vmw_plane_state_to_vps(state);

	/* Should have been freed by cleanup_fb */
	if (vps->surf)
		vmw_surface_unreference(&vps->surf);

	if (vps->bo)
		vmw_bo_unreference(&vps->bo);

	drm_atomic_helper_plane_destroy_state(plane, state);
}


/**
 * vmw_du_connector_duplicate_state - duplicate connector state
 * @connector: DRM connector
 *
 * Allocates and returns a copy of the connector state (both common and
 * vmw-specific) for the specified connector.
 *
 * Returns: The newly allocated connector state, or NULL on failure.
 */
struct drm_connector_state *
vmw_du_connector_duplicate_state(struct drm_connector *connector)
{
	struct drm_connector_state *state;
	struct vmw_connector_state *vcs;

	if (WARN_ON(!connector->state))
		return NULL;

	vcs = kmemdup(connector->state, sizeof(*vcs), GFP_KERNEL);

	if (!vcs)
		return NULL;

	state = &vcs->base;

	__drm_atomic_helper_connector_duplicate_state(connector, state);

	return state;
}


/**
 * vmw_du_connector_reset - creates a blank vmw connector state
 * @connector: DRM connector
 *
 * Resets the atomic state for @connector by freeing the state pointer (which
 * might be NULL, e.g. at driver load time) and allocating a new empty state
 * object.
 */
void vmw_du_connector_reset(struct drm_connector *connector)
{
	struct vmw_connector_state *vcs;


	if (connector->state) {
		__drm_atomic_helper_connector_destroy_state(connector->state);

		kfree(vmw_connector_state_to_vcs(connector->state));
	}

	vcs = kzalloc(sizeof(*vcs), GFP_KERNEL);

	if (!vcs) {
		DRM_ERROR("Cannot allocate vmw_connector_state\n");
		return;
	}

	__drm_atomic_helper_connector_reset(connector, &vcs->base);
}


/**
 * vmw_du_connector_destroy_state - destroy connector state
 * @connector: DRM connector
 * @state: state object to destroy
 *
 * Destroys the connector state (both common and vmw-specific) for the
 * specified plane.
 */
void
vmw_du_connector_destroy_state(struct drm_connector *connector,
			  struct drm_connector_state *state)
{
	drm_atomic_helper_connector_destroy_state(connector, state);
}
/*
 * Generic framebuffer code
 */

/*
 * Surface framebuffer code
 */

static void vmw_framebuffer_surface_destroy(struct drm_framebuffer *framebuffer)
{
	struct vmw_framebuffer_surface *vfbs =
		vmw_framebuffer_to_vfbs(framebuffer);

	drm_framebuffer_cleanup(framebuffer);
	vmw_surface_unreference(&vfbs->surface);

	kfree(vfbs);
}

/**
 * vmw_kms_readback - Perform a readback from the screen system to
 * a buffer-object backed framebuffer.
 *
 * @dev_priv: Pointer to the device private structure.
 * @file_priv: Pointer to a struct drm_file identifying the caller.
 * Must be set to NULL if @user_fence_rep is NULL.
 * @vfb: Pointer to the buffer-object backed framebuffer.
 * @user_fence_rep: User-space provided structure for fence information.
 * Must be set to non-NULL if @file_priv is non-NULL.
 * @vclips: Array of clip rects.
 * @num_clips: Number of clip rects in @vclips.
 *
 * Returns 0 on success, negative error code on failure. -ERESTARTSYS if
 * interrupted.
 */
int vmw_kms_readback(struct vmw_private *dev_priv,
		     struct drm_file *file_priv,
		     struct vmw_framebuffer *vfb,
		     struct drm_vmw_fence_rep __user *user_fence_rep,
		     struct drm_vmw_rect *vclips,
		     uint32_t num_clips)
{
	switch (dev_priv->active_display_unit) {
	case vmw_du_screen_object:
		return vmw_kms_sou_readback(dev_priv, file_priv, vfb,
					    user_fence_rep, vclips, num_clips,
					    NULL);
	case vmw_du_screen_target:
		return vmw_kms_stdu_readback(dev_priv, file_priv, vfb,
					     user_fence_rep, NULL, vclips, num_clips,
					     1, NULL);
	default:
		WARN_ONCE(true,
			  "Readback called with invalid display system.\n");
}

	return -ENOSYS;
}


static const struct drm_framebuffer_funcs vmw_framebuffer_surface_funcs = {
	.destroy = vmw_framebuffer_surface_destroy,
	.dirty = drm_atomic_helper_dirtyfb,
};

static int vmw_kms_new_framebuffer_surface(struct vmw_private *dev_priv,
					   struct vmw_surface *surface,
					   struct vmw_framebuffer **out,
					   const struct drm_mode_fb_cmd2
					   *mode_cmd,
					   bool is_bo_proxy)

{
	struct drm_device *dev = &dev_priv->drm;
	struct vmw_framebuffer_surface *vfbs;
	enum SVGA3dSurfaceFormat format;
	int ret;

	/* 3D is only supported on HWv8 and newer hosts */
	if (dev_priv->active_display_unit == vmw_du_legacy)
		return -ENOSYS;

	/*
	 * Sanity checks.
	 */

	if (!drm_any_plane_has_format(&dev_priv->drm,
				      mode_cmd->pixel_format,
				      mode_cmd->modifier[0])) {
		drm_dbg(&dev_priv->drm,
			"unsupported pixel format %p4cc / modifier 0x%llx\n",
			&mode_cmd->pixel_format, mode_cmd->modifier[0]);
		return -EINVAL;
	}

	/* Surface must be marked as a scanout. */
	if (unlikely(!surface->metadata.scanout))
		return -EINVAL;

	if (unlikely(surface->metadata.mip_levels[0] != 1 ||
		     surface->metadata.num_sizes != 1 ||
		     surface->metadata.base_size.width < mode_cmd->width ||
		     surface->metadata.base_size.height < mode_cmd->height ||
		     surface->metadata.base_size.depth != 1)) {
		DRM_ERROR("Incompatible surface dimensions "
			  "for requested mode.\n");
		return -EINVAL;
	}

	switch (mode_cmd->pixel_format) {
	case DRM_FORMAT_ARGB8888:
		format = SVGA3D_A8R8G8B8;
		break;
	case DRM_FORMAT_XRGB8888:
		format = SVGA3D_X8R8G8B8;
		break;
	case DRM_FORMAT_RGB565:
		format = SVGA3D_R5G6B5;
		break;
	case DRM_FORMAT_XRGB1555:
		format = SVGA3D_A1R5G5B5;
		break;
	default:
		DRM_ERROR("Invalid pixel format: %p4cc\n",
			  &mode_cmd->pixel_format);
		return -EINVAL;
	}

	/*
	 * For DX, surface format validation is done when surface->scanout
	 * is set.
	 */
	if (!has_sm4_context(dev_priv) && format != surface->metadata.format) {
		DRM_ERROR("Invalid surface format for requested mode.\n");
		return -EINVAL;
	}

	vfbs = kzalloc(sizeof(*vfbs), GFP_KERNEL);
	if (!vfbs) {
		ret = -ENOMEM;
		goto out_err1;
	}

	drm_helper_mode_fill_fb_struct(dev, &vfbs->base.base, mode_cmd);
	vfbs->surface = vmw_surface_reference(surface);
	vfbs->base.user_handle = mode_cmd->handles[0];
	vfbs->is_bo_proxy = is_bo_proxy;

	*out = &vfbs->base;

	ret = drm_framebuffer_init(dev, &vfbs->base.base,
				   &vmw_framebuffer_surface_funcs);
	if (ret)
		goto out_err2;

	return 0;

out_err2:
	vmw_surface_unreference(&surface);
	kfree(vfbs);
out_err1:
	return ret;
}

/*
 * Buffer-object framebuffer code
 */

static int vmw_framebuffer_bo_create_handle(struct drm_framebuffer *fb,
					    struct drm_file *file_priv,
					    unsigned int *handle)
{
	struct vmw_framebuffer_bo *vfbd =
			vmw_framebuffer_to_vfbd(fb);

	return drm_gem_handle_create(file_priv, &vfbd->buffer->tbo.base, handle);
}

static void vmw_framebuffer_bo_destroy(struct drm_framebuffer *framebuffer)
{
	struct vmw_framebuffer_bo *vfbd =
		vmw_framebuffer_to_vfbd(framebuffer);

	drm_framebuffer_cleanup(framebuffer);
	vmw_bo_unreference(&vfbd->buffer);

	kfree(vfbd);
}

static const struct drm_framebuffer_funcs vmw_framebuffer_bo_funcs = {
	.create_handle = vmw_framebuffer_bo_create_handle,
	.destroy = vmw_framebuffer_bo_destroy,
	.dirty = drm_atomic_helper_dirtyfb,
};

/**
 * vmw_create_bo_proxy - create a proxy surface for the buffer object
 *
 * @dev: DRM device
 * @mode_cmd: parameters for the new surface
 * @bo_mob: MOB backing the buffer object
 * @srf_out: newly created surface
 *
 * When the content FB is a buffer object, we create a surface as a proxy to the
 * same buffer.  This way we can do a surface copy rather than a surface DMA.
 * This is a more efficient approach
 *
 * RETURNS:
 * 0 on success, error code otherwise
 */
static int vmw_create_bo_proxy(struct drm_device *dev,
			       const struct drm_mode_fb_cmd2 *mode_cmd,
			       struct vmw_bo *bo_mob,
			       struct vmw_surface **srf_out)
{
	struct vmw_surface_metadata metadata = {0};
	uint32_t format;
	struct vmw_resource *res;
	unsigned int bytes_pp;
	int ret;

	switch (mode_cmd->pixel_format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		format = SVGA3D_X8R8G8B8;
		bytes_pp = 4;
		break;

	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB1555:
		format = SVGA3D_R5G6B5;
		bytes_pp = 2;
		break;

	case 8:
		format = SVGA3D_P8;
		bytes_pp = 1;
		break;

	default:
		DRM_ERROR("Invalid framebuffer format %p4cc\n",
			  &mode_cmd->pixel_format);
		return -EINVAL;
	}

	metadata.format = format;
	metadata.mip_levels[0] = 1;
	metadata.num_sizes = 1;
	metadata.base_size.width = mode_cmd->pitches[0] / bytes_pp;
	metadata.base_size.height =  mode_cmd->height;
	metadata.base_size.depth = 1;
	metadata.scanout = true;

	ret = vmw_gb_surface_define(vmw_priv(dev), &metadata, srf_out);
	if (ret) {
		DRM_ERROR("Failed to allocate proxy content buffer\n");
		return ret;
	}

	res = &(*srf_out)->res;

	/* Reserve and switch the backing mob. */
	mutex_lock(&res->dev_priv->cmdbuf_mutex);
	(void) vmw_resource_reserve(res, false, true);
	vmw_bo_unreference(&res->guest_memory_bo);
	res->guest_memory_bo = vmw_bo_reference(bo_mob);
	res->guest_memory_offset = 0;
	vmw_resource_unreserve(res, false, false, false, NULL, 0);
	mutex_unlock(&res->dev_priv->cmdbuf_mutex);

	return 0;
}



static int vmw_kms_new_framebuffer_bo(struct vmw_private *dev_priv,
				      struct vmw_bo *bo,
				      struct vmw_framebuffer **out,
				      const struct drm_mode_fb_cmd2
				      *mode_cmd)

{
	struct drm_device *dev = &dev_priv->drm;
	struct vmw_framebuffer_bo *vfbd;
	unsigned int requested_size;
	int ret;

	requested_size = mode_cmd->height * mode_cmd->pitches[0];
	if (unlikely(requested_size > bo->tbo.base.size)) {
		DRM_ERROR("Screen buffer object size is too small "
			  "for requested mode.\n");
		return -EINVAL;
	}

	if (!drm_any_plane_has_format(&dev_priv->drm,
				      mode_cmd->pixel_format,
				      mode_cmd->modifier[0])) {
		drm_dbg(&dev_priv->drm,
			"unsupported pixel format %p4cc / modifier 0x%llx\n",
			&mode_cmd->pixel_format, mode_cmd->modifier[0]);
		return -EINVAL;
	}

	vfbd = kzalloc(sizeof(*vfbd), GFP_KERNEL);
	if (!vfbd) {
		ret = -ENOMEM;
		goto out_err1;
	}

	vfbd->base.base.obj[0] = &bo->tbo.base;
	drm_helper_mode_fill_fb_struct(dev, &vfbd->base.base, mode_cmd);
	vfbd->base.bo = true;
	vfbd->buffer = vmw_bo_reference(bo);
	vfbd->base.user_handle = mode_cmd->handles[0];
	*out = &vfbd->base;

	ret = drm_framebuffer_init(dev, &vfbd->base.base,
				   &vmw_framebuffer_bo_funcs);
	if (ret)
		goto out_err2;

	return 0;

out_err2:
	vmw_bo_unreference(&bo);
	kfree(vfbd);
out_err1:
	return ret;
}


/**
 * vmw_kms_srf_ok - check if a surface can be created
 *
 * @dev_priv: Pointer to device private struct.
 * @width: requested width
 * @height: requested height
 *
 * Surfaces need to be less than texture size
 */
static bool
vmw_kms_srf_ok(struct vmw_private *dev_priv, uint32_t width, uint32_t height)
{
	if (width  > dev_priv->texture_max_width ||
	    height > dev_priv->texture_max_height)
		return false;

	return true;
}

/**
 * vmw_kms_new_framebuffer - Create a new framebuffer.
 *
 * @dev_priv: Pointer to device private struct.
 * @bo: Pointer to buffer object to wrap the kms framebuffer around.
 * Either @bo or @surface must be NULL.
 * @surface: Pointer to a surface to wrap the kms framebuffer around.
 * Either @bo or @surface must be NULL.
 * @only_2d: No presents will occur to this buffer object based framebuffer.
 * This helps the code to do some important optimizations.
 * @mode_cmd: Frame-buffer metadata.
 */
struct vmw_framebuffer *
vmw_kms_new_framebuffer(struct vmw_private *dev_priv,
			struct vmw_bo *bo,
			struct vmw_surface *surface,
			bool only_2d,
			const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct vmw_framebuffer *vfb = NULL;
	bool is_bo_proxy = false;
	int ret;

	/*
	 * We cannot use the SurfaceDMA command in an non-accelerated VM,
	 * therefore, wrap the buffer object in a surface so we can use the
	 * SurfaceCopy command.
	 */
	if (vmw_kms_srf_ok(dev_priv, mode_cmd->width, mode_cmd->height)  &&
	    bo && only_2d &&
	    mode_cmd->width > 64 &&  /* Don't create a proxy for cursor */
	    dev_priv->active_display_unit == vmw_du_screen_target) {
		ret = vmw_create_bo_proxy(&dev_priv->drm, mode_cmd,
					  bo, &surface);
		if (ret)
			return ERR_PTR(ret);

		is_bo_proxy = true;
	}

	/* Create the new framebuffer depending one what we have */
	if (surface) {
		ret = vmw_kms_new_framebuffer_surface(dev_priv, surface, &vfb,
						      mode_cmd,
						      is_bo_proxy);
		/*
		 * vmw_create_bo_proxy() adds a reference that is no longer
		 * needed
		 */
		if (is_bo_proxy)
			vmw_surface_unreference(&surface);
	} else if (bo) {
		ret = vmw_kms_new_framebuffer_bo(dev_priv, bo, &vfb,
						 mode_cmd);
	} else {
		BUG();
	}

	if (ret)
		return ERR_PTR(ret);

	return vfb;
}

/*
 * Generic Kernel modesetting functions
 */

static struct drm_framebuffer *vmw_kms_fb_create(struct drm_device *dev,
						 struct drm_file *file_priv,
						 const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct vmw_framebuffer *vfb = NULL;
	struct vmw_surface *surface = NULL;
	struct vmw_bo *bo = NULL;
	int ret;

	/* returns either a bo or surface */
	ret = vmw_user_lookup_handle(dev_priv, file_priv,
				     mode_cmd->handles[0],
				     &surface, &bo);
	if (ret) {
		DRM_ERROR("Invalid buffer object handle %u (0x%x).\n",
			  mode_cmd->handles[0], mode_cmd->handles[0]);
		goto err_out;
	}


	if (!bo &&
	    !vmw_kms_srf_ok(dev_priv, mode_cmd->width, mode_cmd->height)) {
		DRM_ERROR("Surface size cannot exceed %dx%d\n",
			dev_priv->texture_max_width,
			dev_priv->texture_max_height);
		goto err_out;
	}


	vfb = vmw_kms_new_framebuffer(dev_priv, bo, surface,
				      !(dev_priv->capabilities & SVGA_CAP_3D),
				      mode_cmd);
	if (IS_ERR(vfb)) {
		ret = PTR_ERR(vfb);
		goto err_out;
	}

err_out:
	/* vmw_user_lookup_handle takes one ref so does new_fb */
	if (bo)
		vmw_user_bo_unref(bo);
	if (surface)
		vmw_surface_unreference(&surface);

	if (ret) {
		DRM_ERROR("failed to create vmw_framebuffer: %i\n", ret);
		return ERR_PTR(ret);
	}

	return &vfb->base;
}

/**
 * vmw_kms_check_display_memory - Validates display memory required for a
 * topology
 * @dev: DRM device
 * @num_rects: number of drm_rect in rects
 * @rects: array of drm_rect representing the topology to validate indexed by
 * crtc index.
 *
 * Returns:
 * 0 on success otherwise negative error code
 */
static int vmw_kms_check_display_memory(struct drm_device *dev,
					uint32_t num_rects,
					struct drm_rect *rects)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct drm_rect bounding_box = {0};
	u64 total_pixels = 0, pixel_mem, bb_mem;
	int i;

	for (i = 0; i < num_rects; i++) {
		/*
		 * For STDU only individual screen (screen target) is limited by
		 * SCREENTARGET_MAX_WIDTH/HEIGHT registers.
		 */
		if (dev_priv->active_display_unit == vmw_du_screen_target &&
		    (drm_rect_width(&rects[i]) > dev_priv->stdu_max_width ||
		     drm_rect_height(&rects[i]) > dev_priv->stdu_max_height)) {
			VMW_DEBUG_KMS("Screen size not supported.\n");
			return -EINVAL;
		}

		/* Bounding box upper left is at (0,0). */
		if (rects[i].x2 > bounding_box.x2)
			bounding_box.x2 = rects[i].x2;

		if (rects[i].y2 > bounding_box.y2)
			bounding_box.y2 = rects[i].y2;

		total_pixels += (u64) drm_rect_width(&rects[i]) *
			(u64) drm_rect_height(&rects[i]);
	}

	/* Virtual svga device primary limits are always in 32-bpp. */
	pixel_mem = total_pixels * 4;

	/*
	 * For HV10 and below prim_bb_mem is vram size. When
	 * SVGA_REG_MAX_PRIMARY_BOUNDING_BOX_MEM is not present vram size is
	 * limit on primary bounding box
	 */
	if (pixel_mem > dev_priv->max_primary_mem) {
		VMW_DEBUG_KMS("Combined output size too large.\n");
		return -EINVAL;
	}

	/* SVGA_CAP_NO_BB_RESTRICTION is available for STDU only. */
	if (dev_priv->active_display_unit != vmw_du_screen_target ||
	    !(dev_priv->capabilities & SVGA_CAP_NO_BB_RESTRICTION)) {
		bb_mem = (u64) bounding_box.x2 * bounding_box.y2 * 4;

		if (bb_mem > dev_priv->max_primary_mem) {
			VMW_DEBUG_KMS("Topology is beyond supported limits.\n");
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * vmw_crtc_state_and_lock - Return new or current crtc state with locked
 * crtc mutex
 * @state: The atomic state pointer containing the new atomic state
 * @crtc: The crtc
 *
 * This function returns the new crtc state if it's part of the state update.
 * Otherwise returns the current crtc state. It also makes sure that the
 * crtc mutex is locked.
 *
 * Returns: A valid crtc state pointer or NULL. It may also return a
 * pointer error, in particular -EDEADLK if locking needs to be rerun.
 */
static struct drm_crtc_state *
vmw_crtc_state_and_lock(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state) {
		lockdep_assert_held(&crtc->mutex.mutex.base);
	} else {
		int ret = drm_modeset_lock(&crtc->mutex, state->acquire_ctx);

		if (ret != 0 && ret != -EALREADY)
			return ERR_PTR(ret);

		crtc_state = crtc->state;
	}

	return crtc_state;
}

/**
 * vmw_kms_check_implicit - Verify that all implicit display units scan out
 * from the same fb after the new state is committed.
 * @dev: The drm_device.
 * @state: The new state to be checked.
 *
 * Returns:
 *   Zero on success,
 *   -EINVAL on invalid state,
 *   -EDEADLK if modeset locking needs to be rerun.
 */
static int vmw_kms_check_implicit(struct drm_device *dev,
				  struct drm_atomic_state *state)
{
	struct drm_framebuffer *implicit_fb = NULL;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *plane_state;

	drm_for_each_crtc(crtc, dev) {
		struct vmw_display_unit *du = vmw_crtc_to_du(crtc);

		if (!du->is_implicit)
			continue;

		crtc_state = vmw_crtc_state_and_lock(state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (!crtc_state || !crtc_state->enable)
			continue;

		/*
		 * Can't move primary planes across crtcs, so this is OK.
		 * It also means we don't need to take the plane mutex.
		 */
		plane_state = du->primary.state;
		if (plane_state->crtc != crtc)
			continue;

		if (!implicit_fb)
			implicit_fb = plane_state->fb;
		else if (implicit_fb != plane_state->fb)
			return -EINVAL;
	}

	return 0;
}

/**
 * vmw_kms_check_topology - Validates topology in drm_atomic_state
 * @dev: DRM device
 * @state: the driver state object
 *
 * Returns:
 * 0 on success otherwise negative error code
 */
static int vmw_kms_check_topology(struct drm_device *dev,
				  struct drm_atomic_state *state)
{
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_rect *rects;
	struct drm_crtc *crtc;
	uint32_t i;
	int ret = 0;

	rects = kcalloc(dev->mode_config.num_crtc, sizeof(struct drm_rect),
			GFP_KERNEL);
	if (!rects)
		return -ENOMEM;

	drm_for_each_crtc(crtc, dev) {
		struct vmw_display_unit *du = vmw_crtc_to_du(crtc);
		struct drm_crtc_state *crtc_state;

		i = drm_crtc_index(crtc);

		crtc_state = vmw_crtc_state_and_lock(state, crtc);
		if (IS_ERR(crtc_state)) {
			ret = PTR_ERR(crtc_state);
			goto clean;
		}

		if (!crtc_state)
			continue;

		if (crtc_state->enable) {
			rects[i].x1 = du->gui_x;
			rects[i].y1 = du->gui_y;
			rects[i].x2 = du->gui_x + crtc_state->mode.hdisplay;
			rects[i].y2 = du->gui_y + crtc_state->mode.vdisplay;
		} else {
			rects[i].x1 = 0;
			rects[i].y1 = 0;
			rects[i].x2 = 0;
			rects[i].y2 = 0;
		}
	}

	/* Determine change to topology due to new atomic state */
	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state,
				      new_crtc_state, i) {
		struct vmw_display_unit *du = vmw_crtc_to_du(crtc);
		struct drm_connector *connector;
		struct drm_connector_state *conn_state;
		struct vmw_connector_state *vmw_conn_state;

		if (!du->pref_active && new_crtc_state->enable) {
			VMW_DEBUG_KMS("Enabling a disabled display unit\n");
			ret = -EINVAL;
			goto clean;
		}

		/*
		 * For vmwgfx each crtc has only one connector attached and it
		 * is not changed so don't really need to check the
		 * crtc->connector_mask and iterate over it.
		 */
		connector = &du->connector;
		conn_state = drm_atomic_get_connector_state(state, connector);
		if (IS_ERR(conn_state)) {
			ret = PTR_ERR(conn_state);
			goto clean;
		}

		vmw_conn_state = vmw_connector_state_to_vcs(conn_state);
		vmw_conn_state->gui_x = du->gui_x;
		vmw_conn_state->gui_y = du->gui_y;
	}

	ret = vmw_kms_check_display_memory(dev, dev->mode_config.num_crtc,
					   rects);

clean:
	kfree(rects);
	return ret;
}

/**
 * vmw_kms_atomic_check_modeset- validate state object for modeset changes
 *
 * @dev: DRM device
 * @state: the driver state object
 *
 * This is a simple wrapper around drm_atomic_helper_check_modeset() for
 * us to assign a value to mode->crtc_clock so that
 * drm_calc_timestamping_constants() won't throw an error message
 *
 * Returns:
 * Zero for success or -errno
 */
static int
vmw_kms_atomic_check_modeset(struct drm_device *dev,
			     struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	bool need_modeset = false;
	int i, ret;

	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	ret = vmw_kms_check_implicit(dev, state);
	if (ret) {
		VMW_DEBUG_KMS("Invalid implicit state\n");
		return ret;
	}

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		if (drm_atomic_crtc_needs_modeset(crtc_state))
			need_modeset = true;
	}

	if (need_modeset)
		return vmw_kms_check_topology(dev, state);

	return ret;
}

static const struct drm_mode_config_funcs vmw_kms_funcs = {
	.fb_create = vmw_kms_fb_create,
	.atomic_check = vmw_kms_atomic_check_modeset,
	.atomic_commit = drm_atomic_helper_commit,
};

static int vmw_kms_generic_present(struct vmw_private *dev_priv,
				   struct drm_file *file_priv,
				   struct vmw_framebuffer *vfb,
				   struct vmw_surface *surface,
				   uint32_t sid,
				   int32_t destX, int32_t destY,
				   struct drm_vmw_rect *clips,
				   uint32_t num_clips)
{
	return vmw_kms_sou_do_surface_dirty(dev_priv, vfb, NULL, clips,
					    &surface->res, destX, destY,
					    num_clips, 1, NULL, NULL);
}


int vmw_kms_present(struct vmw_private *dev_priv,
		    struct drm_file *file_priv,
		    struct vmw_framebuffer *vfb,
		    struct vmw_surface *surface,
		    uint32_t sid,
		    int32_t destX, int32_t destY,
		    struct drm_vmw_rect *clips,
		    uint32_t num_clips)
{
	int ret;

	switch (dev_priv->active_display_unit) {
	case vmw_du_screen_target:
		ret = vmw_kms_stdu_surface_dirty(dev_priv, vfb, NULL, clips,
						 &surface->res, destX, destY,
						 num_clips, 1, NULL, NULL);
		break;
	case vmw_du_screen_object:
		ret = vmw_kms_generic_present(dev_priv, file_priv, vfb, surface,
					      sid, destX, destY, clips,
					      num_clips);
		break;
	default:
		WARN_ONCE(true,
			  "Present called with invalid display system.\n");
		ret = -ENOSYS;
		break;
	}
	if (ret)
		return ret;

	vmw_cmd_flush(dev_priv, false);

	return 0;
}

static void
vmw_kms_create_hotplug_mode_update_property(struct vmw_private *dev_priv)
{
	if (dev_priv->hotplug_mode_update_property)
		return;

	dev_priv->hotplug_mode_update_property =
		drm_property_create_range(&dev_priv->drm,
					  DRM_MODE_PROP_IMMUTABLE,
					  "hotplug_mode_update", 0, 1);
}

int vmw_kms_init(struct vmw_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	int ret;
	static const char *display_unit_names[] = {
		"Invalid",
		"Legacy",
		"Screen Object",
		"Screen Target",
		"Invalid (max)"
	};

	drm_mode_config_init(dev);
	dev->mode_config.funcs = &vmw_kms_funcs;
	dev->mode_config.min_width = 1;
	dev->mode_config.min_height = 1;
	dev->mode_config.max_width = dev_priv->texture_max_width;
	dev->mode_config.max_height = dev_priv->texture_max_height;
	dev->mode_config.preferred_depth = dev_priv->assume_16bpp ? 16 : 32;

	drm_mode_create_suggested_offset_properties(dev);
	vmw_kms_create_hotplug_mode_update_property(dev_priv);

	ret = vmw_kms_stdu_init_display(dev_priv);
	if (ret) {
		ret = vmw_kms_sou_init_display(dev_priv);
		if (ret) /* Fallback */
			ret = vmw_kms_ldu_init_display(dev_priv);
	}
	BUILD_BUG_ON(ARRAY_SIZE(display_unit_names) != (vmw_du_max + 1));
	drm_info(&dev_priv->drm, "%s display unit initialized\n",
		 display_unit_names[dev_priv->active_display_unit]);

	return ret;
}

int vmw_kms_close(struct vmw_private *dev_priv)
{
	int ret = 0;

	/*
	 * Docs says we should take the lock before calling this function
	 * but since it destroys encoders and our destructor calls
	 * drm_encoder_cleanup which takes the lock we deadlock.
	 */
	drm_mode_config_cleanup(&dev_priv->drm);
	if (dev_priv->active_display_unit == vmw_du_legacy)
		ret = vmw_kms_ldu_close_display(dev_priv);

	return ret;
}

int vmw_kms_cursor_bypass_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_vmw_cursor_bypass_arg *arg = data;
	struct vmw_display_unit *du;
	struct drm_crtc *crtc;
	int ret = 0;

	mutex_lock(&dev->mode_config.mutex);
	if (arg->flags & DRM_VMW_CURSOR_BYPASS_ALL) {

		list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
			du = vmw_crtc_to_du(crtc);
			du->hotspot_x = arg->xhot;
			du->hotspot_y = arg->yhot;
		}

		mutex_unlock(&dev->mode_config.mutex);
		return 0;
	}

	crtc = drm_crtc_find(dev, file_priv, arg->crtc_id);
	if (!crtc) {
		ret = -ENOENT;
		goto out;
	}

	du = vmw_crtc_to_du(crtc);

	du->hotspot_x = arg->xhot;
	du->hotspot_y = arg->yhot;

out:
	mutex_unlock(&dev->mode_config.mutex);

	return ret;
}

int vmw_kms_write_svga(struct vmw_private *vmw_priv,
			unsigned width, unsigned height, unsigned pitch,
			unsigned bpp, unsigned depth)
{
	if (vmw_priv->capabilities & SVGA_CAP_PITCHLOCK)
		vmw_write(vmw_priv, SVGA_REG_PITCHLOCK, pitch);
	else if (vmw_fifo_have_pitchlock(vmw_priv))
		vmw_fifo_mem_write(vmw_priv, SVGA_FIFO_PITCHLOCK, pitch);
	vmw_write(vmw_priv, SVGA_REG_WIDTH, width);
	vmw_write(vmw_priv, SVGA_REG_HEIGHT, height);
	if ((vmw_priv->capabilities & SVGA_CAP_8BIT_EMULATION) != 0)
		vmw_write(vmw_priv, SVGA_REG_BITS_PER_PIXEL, bpp);

	if (vmw_read(vmw_priv, SVGA_REG_DEPTH) != depth) {
		DRM_ERROR("Invalid depth %u for %u bpp, host expects %u\n",
			  depth, bpp, vmw_read(vmw_priv, SVGA_REG_DEPTH));
		return -EINVAL;
	}

	return 0;
}

bool vmw_kms_validate_mode_vram(struct vmw_private *dev_priv,
				uint32_t pitch,
				uint32_t height)
{
	return ((u64) pitch * (u64) height) < (u64)
		((dev_priv->active_display_unit == vmw_du_screen_target) ?
		 dev_priv->max_primary_mem : dev_priv->vram_size);
}

/**
 * vmw_du_update_layout - Update the display unit with topology from resolution
 * plugin and generate DRM uevent
 * @dev_priv: device private
 * @num_rects: number of drm_rect in rects
 * @rects: toplogy to update
 */
static int vmw_du_update_layout(struct vmw_private *dev_priv,
				unsigned int num_rects, struct drm_rect *rects)
{
	struct drm_device *dev = &dev_priv->drm;
	struct vmw_display_unit *du;
	struct drm_connector *con;
	struct drm_connector_list_iter conn_iter;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc *crtc;
	int ret;

	/* Currently gui_x/y is protected with the crtc mutex */
	mutex_lock(&dev->mode_config.mutex);
	drm_modeset_acquire_init(&ctx, 0);
retry:
	drm_for_each_crtc(crtc, dev) {
		ret = drm_modeset_lock(&crtc->mutex, &ctx);
		if (ret < 0) {
			if (ret == -EDEADLK) {
				drm_modeset_backoff(&ctx);
				goto retry;
		}
			goto out_fini;
		}
	}

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(con, &conn_iter) {
		du = vmw_connector_to_du(con);
		if (num_rects > du->unit) {
			du->pref_width = drm_rect_width(&rects[du->unit]);
			du->pref_height = drm_rect_height(&rects[du->unit]);
			du->pref_active = true;
			du->gui_x = rects[du->unit].x1;
			du->gui_y = rects[du->unit].y1;
		} else {
			du->pref_width  = VMWGFX_MIN_INITIAL_WIDTH;
			du->pref_height = VMWGFX_MIN_INITIAL_HEIGHT;
			du->pref_active = false;
			du->gui_x = 0;
			du->gui_y = 0;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	list_for_each_entry(con, &dev->mode_config.connector_list, head) {
		du = vmw_connector_to_du(con);
		if (num_rects > du->unit) {
			drm_object_property_set_value
			  (&con->base, dev->mode_config.suggested_x_property,
			   du->gui_x);
			drm_object_property_set_value
			  (&con->base, dev->mode_config.suggested_y_property,
			   du->gui_y);
		} else {
			drm_object_property_set_value
			  (&con->base, dev->mode_config.suggested_x_property,
			   0);
			drm_object_property_set_value
			  (&con->base, dev->mode_config.suggested_y_property,
			   0);
		}
		con->status = vmw_du_connector_detect(con, true);
	}
out_fini:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	mutex_unlock(&dev->mode_config.mutex);

	drm_sysfs_hotplug_event(dev);

	return 0;
}

int vmw_du_crtc_gamma_set(struct drm_crtc *crtc,
			  u16 *r, u16 *g, u16 *b,
			  uint32_t size,
			  struct drm_modeset_acquire_ctx *ctx)
{
	struct vmw_private *dev_priv = vmw_priv(crtc->dev);
	int i;

	for (i = 0; i < size; i++) {
		DRM_DEBUG("%d r/g/b = 0x%04x / 0x%04x / 0x%04x\n", i,
			  r[i], g[i], b[i]);
		vmw_write(dev_priv, SVGA_PALETTE_BASE + i * 3 + 0, r[i] >> 8);
		vmw_write(dev_priv, SVGA_PALETTE_BASE + i * 3 + 1, g[i] >> 8);
		vmw_write(dev_priv, SVGA_PALETTE_BASE + i * 3 + 2, b[i] >> 8);
	}

	return 0;
}

int vmw_du_connector_dpms(struct drm_connector *connector, int mode)
{
	return 0;
}

enum drm_connector_status
vmw_du_connector_detect(struct drm_connector *connector, bool force)
{
	uint32_t num_displays;
	struct drm_device *dev = connector->dev;
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct vmw_display_unit *du = vmw_connector_to_du(connector);

	num_displays = vmw_read(dev_priv, SVGA_REG_NUM_DISPLAYS);

	return ((vmw_connector_to_du(connector)->unit < num_displays &&
		 du->pref_active) ?
		connector_status_connected : connector_status_disconnected);
}

static struct drm_display_mode vmw_kms_connector_builtin[] = {
	/* 640x480@60Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 489, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 800x600@60Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840,
		   968, 1056, 0, 600, 601, 605, 628, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1024x768@60Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 1152x864@75Hz */
	{ DRM_MODE("1152x864", DRM_MODE_TYPE_DRIVER, 108000, 1152, 1216,
		   1344, 1600, 0, 864, 865, 868, 900, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1280x720@60Hz */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74500, 1280, 1344,
		   1472, 1664, 0, 720, 723, 728, 748, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1280x768@60Hz */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 79500, 1280, 1344,
		   1472, 1664, 0, 768, 771, 778, 798, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1280x800@60Hz */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 83500, 1280, 1352,
		   1480, 1680, 0, 800, 803, 809, 831, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 1280x960@60Hz */
	{ DRM_MODE("1280x960", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1376,
		   1488, 1800, 0, 960, 961, 964, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1280x1024@60Hz */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1328,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1360x768@60Hz */
	{ DRM_MODE("1360x768", DRM_MODE_TYPE_DRIVER, 85500, 1360, 1424,
		   1536, 1792, 0, 768, 771, 777, 795, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1440x1050@60Hz */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 121750, 1400, 1488,
		   1632, 1864, 0, 1050, 1053, 1057, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1440x900@60Hz */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 106500, 1440, 1520,
		   1672, 1904, 0, 900, 903, 909, 934, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1600x1200@60Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 162000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1680x1050@60Hz */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 146250, 1680, 1784,
		   1960, 2240, 0, 1050, 1053, 1059, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1792x1344@60Hz */
	{ DRM_MODE("1792x1344", DRM_MODE_TYPE_DRIVER, 204750, 1792, 1920,
		   2120, 2448, 0, 1344, 1345, 1348, 1394, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1853x1392@60Hz */
	{ DRM_MODE("1856x1392", DRM_MODE_TYPE_DRIVER, 218250, 1856, 1952,
		   2176, 2528, 0, 1392, 1393, 1396, 1439, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1920x1080@60Hz */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 173000, 1920, 2048,
		   2248, 2576, 0, 1080, 1083, 1088, 1120, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1920x1200@60Hz */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 193250, 1920, 2056,
		   2256, 2592, 0, 1200, 1203, 1209, 1245, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 1920x1440@60Hz */
	{ DRM_MODE("1920x1440", DRM_MODE_TYPE_DRIVER, 234000, 1920, 2048,
		   2256, 2600, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 2560x1440@60Hz */
	{ DRM_MODE("2560x1440", DRM_MODE_TYPE_DRIVER, 241500, 2560, 2608,
		   2640, 2720, 0, 1440, 1443, 1448, 1481, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 2560x1600@60Hz */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 348500, 2560, 2752,
		   3032, 3504, 0, 1600, 1603, 1609, 1658, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 2880x1800@60Hz */
	{ DRM_MODE("2880x1800", DRM_MODE_TYPE_DRIVER, 337500, 2880, 2928,
		   2960, 3040, 0, 1800, 1803, 1809, 1852, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 3840x2160@60Hz */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 533000, 3840, 3888,
		   3920, 4000, 0, 2160, 2163, 2168, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 3840x2400@60Hz */
	{ DRM_MODE("3840x2400", DRM_MODE_TYPE_DRIVER, 592250, 3840, 3888,
		   3920, 4000, 0, 2400, 2403, 2409, 2469, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* Terminate */
	{ DRM_MODE("", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) },
};

/**
 * vmw_guess_mode_timing - Provide fake timings for a
 * 60Hz vrefresh mode.
 *
 * @mode: Pointer to a struct drm_display_mode with hdisplay and vdisplay
 * members filled in.
 */
void vmw_guess_mode_timing(struct drm_display_mode *mode)
{
	mode->hsync_start = mode->hdisplay + 50;
	mode->hsync_end = mode->hsync_start + 50;
	mode->htotal = mode->hsync_end + 50;

	mode->vsync_start = mode->vdisplay + 50;
	mode->vsync_end = mode->vsync_start + 50;
	mode->vtotal = mode->vsync_end + 50;

	mode->clock = (u32)mode->htotal * (u32)mode->vtotal / 100 * 6;
}


int vmw_du_connector_fill_modes(struct drm_connector *connector,
				uint32_t max_width, uint32_t max_height)
{
	struct vmw_display_unit *du = vmw_connector_to_du(connector);
	struct drm_device *dev = connector->dev;
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct drm_display_mode *mode = NULL;
	struct drm_display_mode *bmode;
	struct drm_display_mode prefmode = { DRM_MODE("preferred",
		DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC)
	};
	int i;
	u32 assumed_bpp = 4;

	if (dev_priv->assume_16bpp)
		assumed_bpp = 2;

	max_width  = min(max_width,  dev_priv->texture_max_width);
	max_height = min(max_height, dev_priv->texture_max_height);

	/*
	 * For STDU extra limit for a mode on SVGA_REG_SCREENTARGET_MAX_WIDTH/
	 * HEIGHT registers.
	 */
	if (dev_priv->active_display_unit == vmw_du_screen_target) {
		max_width  = min(max_width,  dev_priv->stdu_max_width);
		max_height = min(max_height, dev_priv->stdu_max_height);
	}

	/* Add preferred mode */
	mode = drm_mode_duplicate(dev, &prefmode);
	if (!mode)
		return 0;
	mode->hdisplay = du->pref_width;
	mode->vdisplay = du->pref_height;
	vmw_guess_mode_timing(mode);
	drm_mode_set_name(mode);

	if (vmw_kms_validate_mode_vram(dev_priv,
					mode->hdisplay * assumed_bpp,
					mode->vdisplay)) {
		drm_mode_probed_add(connector, mode);
	} else {
		drm_mode_destroy(dev, mode);
		mode = NULL;
	}

	if (du->pref_mode) {
		list_del_init(&du->pref_mode->head);
		drm_mode_destroy(dev, du->pref_mode);
	}

	/* mode might be null here, this is intended */
	du->pref_mode = mode;

	for (i = 0; vmw_kms_connector_builtin[i].type != 0; i++) {
		bmode = &vmw_kms_connector_builtin[i];
		if (bmode->hdisplay > max_width ||
		    bmode->vdisplay > max_height)
			continue;

		if (!vmw_kms_validate_mode_vram(dev_priv,
						bmode->hdisplay * assumed_bpp,
						bmode->vdisplay))
			continue;

		mode = drm_mode_duplicate(dev, bmode);
		if (!mode)
			return 0;

		drm_mode_probed_add(connector, mode);
	}

	drm_connector_list_update(connector);
	/* Move the prefered mode first, help apps pick the right mode. */
	drm_mode_sort(&connector->modes);

	return 1;
}

/**
 * vmw_kms_update_layout_ioctl - Handler for DRM_VMW_UPDATE_LAYOUT ioctl
 * @dev: drm device for the ioctl
 * @data: data pointer for the ioctl
 * @file_priv: drm file for the ioctl call
 *
 * Update preferred topology of display unit as per ioctl request. The topology
 * is expressed as array of drm_vmw_rect.
 * e.g.
 * [0 0 640 480] [640 0 800 600] [0 480 640 480]
 *
 * NOTE:
 * The x and y offset (upper left) in drm_vmw_rect cannot be less than 0. Beside
 * device limit on topology, x + w and y + h (lower right) cannot be greater
 * than INT_MAX. So topology beyond these limits will return with error.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int vmw_kms_update_layout_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct drm_mode_config *mode_config = &dev->mode_config;
	struct drm_vmw_update_layout_arg *arg =
		(struct drm_vmw_update_layout_arg *)data;
	void __user *user_rects;
	struct drm_vmw_rect *rects;
	struct drm_rect *drm_rects;
	unsigned rects_size;
	int ret, i;

	if (!arg->num_outputs) {
		struct drm_rect def_rect = {0, 0,
					    VMWGFX_MIN_INITIAL_WIDTH,
					    VMWGFX_MIN_INITIAL_HEIGHT};
		vmw_du_update_layout(dev_priv, 1, &def_rect);
		return 0;
	}

	rects_size = arg->num_outputs * sizeof(struct drm_vmw_rect);
	rects = kcalloc(arg->num_outputs, sizeof(struct drm_vmw_rect),
			GFP_KERNEL);
	if (unlikely(!rects))
		return -ENOMEM;

	user_rects = (void __user *)(unsigned long)arg->rects;
	ret = copy_from_user(rects, user_rects, rects_size);
	if (unlikely(ret != 0)) {
		DRM_ERROR("Failed to get rects.\n");
		ret = -EFAULT;
		goto out_free;
	}

	drm_rects = (struct drm_rect *)rects;

	VMW_DEBUG_KMS("Layout count = %u\n", arg->num_outputs);
	for (i = 0; i < arg->num_outputs; i++) {
		struct drm_vmw_rect curr_rect;

		/* Verify user-space for overflow as kernel use drm_rect */
		if ((rects[i].x + rects[i].w > INT_MAX) ||
		    (rects[i].y + rects[i].h > INT_MAX)) {
			ret = -ERANGE;
			goto out_free;
		}

		curr_rect = rects[i];
		drm_rects[i].x1 = curr_rect.x;
		drm_rects[i].y1 = curr_rect.y;
		drm_rects[i].x2 = curr_rect.x + curr_rect.w;
		drm_rects[i].y2 = curr_rect.y + curr_rect.h;

		VMW_DEBUG_KMS("  x1 = %d y1 = %d x2 = %d y2 = %d\n",
			      drm_rects[i].x1, drm_rects[i].y1,
			      drm_rects[i].x2, drm_rects[i].y2);

		/*
		 * Currently this check is limiting the topology within
		 * mode_config->max (which actually is max texture size
		 * supported by virtual device). This limit is here to address
		 * window managers that create a big framebuffer for whole
		 * topology.
		 */
		if (drm_rects[i].x1 < 0 ||  drm_rects[i].y1 < 0 ||
		    drm_rects[i].x2 > mode_config->max_width ||
		    drm_rects[i].y2 > mode_config->max_height) {
			VMW_DEBUG_KMS("Invalid layout %d %d %d %d\n",
				      drm_rects[i].x1, drm_rects[i].y1,
				      drm_rects[i].x2, drm_rects[i].y2);
			ret = -EINVAL;
			goto out_free;
		}
	}

	ret = vmw_kms_check_display_memory(dev, arg->num_outputs, drm_rects);

	if (ret == 0)
		vmw_du_update_layout(dev_priv, arg->num_outputs, drm_rects);

out_free:
	kfree(rects);
	return ret;
}

/**
 * vmw_kms_helper_dirty - Helper to build commands and perform actions based
 * on a set of cliprects and a set of display units.
 *
 * @dev_priv: Pointer to a device private structure.
 * @framebuffer: Pointer to the framebuffer on which to perform the actions.
 * @clips: A set of struct drm_clip_rect. Either this os @vclips must be NULL.
 * Cliprects are given in framebuffer coordinates.
 * @vclips: A set of struct drm_vmw_rect cliprects. Either this or @clips must
 * be NULL. Cliprects are given in source coordinates.
 * @dest_x: X coordinate offset for the crtc / destination clip rects.
 * @dest_y: Y coordinate offset for the crtc / destination clip rects.
 * @num_clips: Number of cliprects in the @clips or @vclips array.
 * @increment: Integer with which to increment the clip counter when looping.
 * Used to skip a predetermined number of clip rects.
 * @dirty: Closure structure. See the description of struct vmw_kms_dirty.
 */
int vmw_kms_helper_dirty(struct vmw_private *dev_priv,
			 struct vmw_framebuffer *framebuffer,
			 const struct drm_clip_rect *clips,
			 const struct drm_vmw_rect *vclips,
			 s32 dest_x, s32 dest_y,
			 int num_clips,
			 int increment,
			 struct vmw_kms_dirty *dirty)
{
	struct vmw_display_unit *units[VMWGFX_NUM_DISPLAY_UNITS];
	struct drm_crtc *crtc;
	u32 num_units = 0;
	u32 i, k;

	dirty->dev_priv = dev_priv;

	/* If crtc is passed, no need to iterate over other display units */
	if (dirty->crtc) {
		units[num_units++] = vmw_crtc_to_du(dirty->crtc);
	} else {
		list_for_each_entry(crtc, &dev_priv->drm.mode_config.crtc_list,
				    head) {
			struct drm_plane *plane = crtc->primary;

			if (plane->state->fb == &framebuffer->base)
				units[num_units++] = vmw_crtc_to_du(crtc);
		}
	}

	for (k = 0; k < num_units; k++) {
		struct vmw_display_unit *unit = units[k];
		s32 crtc_x = unit->crtc.x;
		s32 crtc_y = unit->crtc.y;
		s32 crtc_width = unit->crtc.mode.hdisplay;
		s32 crtc_height = unit->crtc.mode.vdisplay;
		const struct drm_clip_rect *clips_ptr = clips;
		const struct drm_vmw_rect *vclips_ptr = vclips;

		dirty->unit = unit;
		if (dirty->fifo_reserve_size > 0) {
			dirty->cmd = VMW_CMD_RESERVE(dev_priv,
						      dirty->fifo_reserve_size);
			if (!dirty->cmd)
				return -ENOMEM;

			memset(dirty->cmd, 0, dirty->fifo_reserve_size);
		}
		dirty->num_hits = 0;
		for (i = 0; i < num_clips; i++, clips_ptr += increment,
		       vclips_ptr += increment) {
			s32 clip_left;
			s32 clip_top;

			/*
			 * Select clip array type. Note that integer type
			 * in @clips is unsigned short, whereas in @vclips
			 * it's 32-bit.
			 */
			if (clips) {
				dirty->fb_x = (s32) clips_ptr->x1;
				dirty->fb_y = (s32) clips_ptr->y1;
				dirty->unit_x2 = (s32) clips_ptr->x2 + dest_x -
					crtc_x;
				dirty->unit_y2 = (s32) clips_ptr->y2 + dest_y -
					crtc_y;
			} else {
				dirty->fb_x = vclips_ptr->x;
				dirty->fb_y = vclips_ptr->y;
				dirty->unit_x2 = dirty->fb_x + vclips_ptr->w +
					dest_x - crtc_x;
				dirty->unit_y2 = dirty->fb_y + vclips_ptr->h +
					dest_y - crtc_y;
			}

			dirty->unit_x1 = dirty->fb_x + dest_x - crtc_x;
			dirty->unit_y1 = dirty->fb_y + dest_y - crtc_y;

			/* Skip this clip if it's outside the crtc region */
			if (dirty->unit_x1 >= crtc_width ||
			    dirty->unit_y1 >= crtc_height ||
			    dirty->unit_x2 <= 0 || dirty->unit_y2 <= 0)
				continue;

			/* Clip right and bottom to crtc limits */
			dirty->unit_x2 = min_t(s32, dirty->unit_x2,
					       crtc_width);
			dirty->unit_y2 = min_t(s32, dirty->unit_y2,
					       crtc_height);

			/* Clip left and top to crtc limits */
			clip_left = min_t(s32, dirty->unit_x1, 0);
			clip_top = min_t(s32, dirty->unit_y1, 0);
			dirty->unit_x1 -= clip_left;
			dirty->unit_y1 -= clip_top;
			dirty->fb_x -= clip_left;
			dirty->fb_y -= clip_top;

			dirty->clip(dirty);
		}

		dirty->fifo_commit(dirty);
	}

	return 0;
}

/**
 * vmw_kms_helper_validation_finish - Helper for post KMS command submission
 * cleanup and fencing
 * @dev_priv: Pointer to the device-private struct
 * @file_priv: Pointer identifying the client when user-space fencing is used
 * @ctx: Pointer to the validation context
 * @out_fence: If non-NULL, returned refcounted fence-pointer
 * @user_fence_rep: If non-NULL, pointer to user-space address area
 * in which to copy user-space fence info
 */
void vmw_kms_helper_validation_finish(struct vmw_private *dev_priv,
				      struct drm_file *file_priv,
				      struct vmw_validation_context *ctx,
				      struct vmw_fence_obj **out_fence,
				      struct drm_vmw_fence_rep __user *
				      user_fence_rep)
{
	struct vmw_fence_obj *fence = NULL;
	uint32_t handle = 0;
	int ret = 0;

	if (file_priv || user_fence_rep || vmw_validation_has_bos(ctx) ||
	    out_fence)
		ret = vmw_execbuf_fence_commands(file_priv, dev_priv, &fence,
						 file_priv ? &handle : NULL);
	vmw_validation_done(ctx, fence);
	if (file_priv)
		vmw_execbuf_copy_fence_user(dev_priv, vmw_fpriv(file_priv),
					    ret, user_fence_rep, fence,
					    handle, -1);
	if (out_fence)
		*out_fence = fence;
	else
		vmw_fence_obj_unreference(&fence);
}

/**
 * vmw_kms_update_proxy - Helper function to update a proxy surface from
 * its backing MOB.
 *
 * @res: Pointer to the surface resource
 * @clips: Clip rects in framebuffer (surface) space.
 * @num_clips: Number of clips in @clips.
 * @increment: Integer with which to increment the clip counter when looping.
 * Used to skip a predetermined number of clip rects.
 *
 * This function makes sure the proxy surface is updated from its backing MOB
 * using the region given by @clips. The surface resource @res and its backing
 * MOB needs to be reserved and validated on call.
 */
int vmw_kms_update_proxy(struct vmw_resource *res,
			 const struct drm_clip_rect *clips,
			 unsigned num_clips,
			 int increment)
{
	struct vmw_private *dev_priv = res->dev_priv;
	struct drm_vmw_size *size = &vmw_res_to_srf(res)->metadata.base_size;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdUpdateGBImage body;
	} *cmd;
	SVGA3dBox *box;
	size_t copy_size = 0;
	int i;

	if (!clips)
		return 0;

	cmd = VMW_CMD_RESERVE(dev_priv, sizeof(*cmd) * num_clips);
	if (!cmd)
		return -ENOMEM;

	for (i = 0; i < num_clips; ++i, clips += increment, ++cmd) {
		box = &cmd->body.box;

		cmd->header.id = SVGA_3D_CMD_UPDATE_GB_IMAGE;
		cmd->header.size = sizeof(cmd->body);
		cmd->body.image.sid = res->id;
		cmd->body.image.face = 0;
		cmd->body.image.mipmap = 0;

		if (clips->x1 > size->width || clips->x2 > size->width ||
		    clips->y1 > size->height || clips->y2 > size->height) {
			DRM_ERROR("Invalid clips outsize of framebuffer.\n");
			return -EINVAL;
		}

		box->x = clips->x1;
		box->y = clips->y1;
		box->z = 0;
		box->w = clips->x2 - clips->x1;
		box->h = clips->y2 - clips->y1;
		box->d = 1;

		copy_size += sizeof(*cmd);
	}

	vmw_cmd_commit(dev_priv, copy_size);

	return 0;
}

/**
 * vmw_kms_create_implicit_placement_property - Set up the implicit placement
 * property.
 *
 * @dev_priv: Pointer to a device private struct.
 *
 * Sets up the implicit placement property unless it's already set up.
 */
void
vmw_kms_create_implicit_placement_property(struct vmw_private *dev_priv)
{
	if (dev_priv->implicit_placement_property)
		return;

	dev_priv->implicit_placement_property =
		drm_property_create_range(&dev_priv->drm,
					  DRM_MODE_PROP_IMMUTABLE,
					  "implicit_placement", 0, 1);
}

/**
 * vmw_kms_suspend - Save modesetting state and turn modesetting off.
 *
 * @dev: Pointer to the drm device
 * Return: 0 on success. Negative error code on failure.
 */
int vmw_kms_suspend(struct drm_device *dev)
{
	struct vmw_private *dev_priv = vmw_priv(dev);

	dev_priv->suspend_state = drm_atomic_helper_suspend(dev);
	if (IS_ERR(dev_priv->suspend_state)) {
		int ret = PTR_ERR(dev_priv->suspend_state);

		DRM_ERROR("Failed kms suspend: %d\n", ret);
		dev_priv->suspend_state = NULL;

		return ret;
	}

	return 0;
}


/**
 * vmw_kms_resume - Re-enable modesetting and restore state
 *
 * @dev: Pointer to the drm device
 * Return: 0 on success. Negative error code on failure.
 *
 * State is resumed from a previous vmw_kms_suspend(). It's illegal
 * to call this function without a previous vmw_kms_suspend().
 */
int vmw_kms_resume(struct drm_device *dev)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	int ret;

	if (WARN_ON(!dev_priv->suspend_state))
		return 0;

	ret = drm_atomic_helper_resume(dev, dev_priv->suspend_state);
	dev_priv->suspend_state = NULL;

	return ret;
}

/**
 * vmw_kms_lost_device - Notify kms that modesetting capabilities will be lost
 *
 * @dev: Pointer to the drm device
 */
void vmw_kms_lost_device(struct drm_device *dev)
{
	drm_atomic_helper_shutdown(dev);
}

/**
 * vmw_du_helper_plane_update - Helper to do plane update on a display unit.
 * @update: The closure structure.
 *
 * Call this helper after setting callbacks in &vmw_du_update_plane to do plane
 * update on display unit.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int vmw_du_helper_plane_update(struct vmw_du_update_plane *update)
{
	struct drm_plane_state *state = update->plane->state;
	struct drm_plane_state *old_state = update->old_state;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	struct drm_rect bb;
	DECLARE_VAL_CONTEXT(val_ctx, NULL, 0);
	uint32_t reserved_size = 0;
	uint32_t submit_size = 0;
	uint32_t curr_size = 0;
	uint32_t num_hits = 0;
	void *cmd_start;
	char *cmd_next;
	int ret;

	/*
	 * Iterate in advance to check if really need plane update and find the
	 * number of clips that actually are in plane src for fifo allocation.
	 */
	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip)
		num_hits++;

	if (num_hits == 0)
		return 0;

	if (update->vfb->bo) {
		struct vmw_framebuffer_bo *vfbbo =
			container_of(update->vfb, typeof(*vfbbo), base);

		/*
		 * For screen targets we want a mappable bo, for everything else we want
		 * accelerated i.e. host backed (vram or gmr) bo. If the display unit
		 * is not screen target then mob's shouldn't be available.
		 */
		if (update->dev_priv->active_display_unit == vmw_du_screen_target) {
			vmw_bo_placement_set(vfbbo->buffer,
					     VMW_BO_DOMAIN_SYS | VMW_BO_DOMAIN_MOB | VMW_BO_DOMAIN_GMR,
					     VMW_BO_DOMAIN_SYS | VMW_BO_DOMAIN_MOB | VMW_BO_DOMAIN_GMR);
		} else {
			WARN_ON(update->dev_priv->has_mob);
			vmw_bo_placement_set_default_accelerated(vfbbo->buffer);
		}
		ret = vmw_validation_add_bo(&val_ctx, vfbbo->buffer);
	} else {
		struct vmw_framebuffer_surface *vfbs =
			container_of(update->vfb, typeof(*vfbs), base);

		ret = vmw_validation_add_resource(&val_ctx, &vfbs->surface->res,
						  0, VMW_RES_DIRTY_NONE, NULL,
						  NULL);
	}

	if (ret)
		return ret;

	ret = vmw_validation_prepare(&val_ctx, update->mutex, update->intr);
	if (ret)
		goto out_unref;

	reserved_size = update->calc_fifo_size(update, num_hits);
	cmd_start = VMW_CMD_RESERVE(update->dev_priv, reserved_size);
	if (!cmd_start) {
		ret = -ENOMEM;
		goto out_revert;
	}

	cmd_next = cmd_start;

	if (update->post_prepare) {
		curr_size = update->post_prepare(update, cmd_next);
		cmd_next += curr_size;
		submit_size += curr_size;
	}

	if (update->pre_clip) {
		curr_size = update->pre_clip(update, cmd_next, num_hits);
		cmd_next += curr_size;
		submit_size += curr_size;
	}

	bb.x1 = INT_MAX;
	bb.y1 = INT_MAX;
	bb.x2 = INT_MIN;
	bb.y2 = INT_MIN;

	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		uint32_t fb_x = clip.x1;
		uint32_t fb_y = clip.y1;

		vmw_du_translate_to_crtc(state, &clip);
		if (update->clip) {
			curr_size = update->clip(update, cmd_next, &clip, fb_x,
						 fb_y);
			cmd_next += curr_size;
			submit_size += curr_size;
		}
		bb.x1 = min_t(int, bb.x1, clip.x1);
		bb.y1 = min_t(int, bb.y1, clip.y1);
		bb.x2 = max_t(int, bb.x2, clip.x2);
		bb.y2 = max_t(int, bb.y2, clip.y2);
	}

	curr_size = update->post_clip(update, cmd_next, &bb);
	submit_size += curr_size;

	if (reserved_size < submit_size)
		submit_size = 0;

	vmw_cmd_commit(update->dev_priv, submit_size);

	vmw_kms_helper_validation_finish(update->dev_priv, NULL, &val_ctx,
					 update->out_fence, NULL);
	return ret;

out_revert:
	vmw_validation_revert(&val_ctx);

out_unref:
	vmw_validation_unref_lists(&val_ctx);
	return ret;
}
