/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __DRM_GEM_ATOMIC_HELPER_H__
#define __DRM_GEM_ATOMIC_HELPER_H__

#include <linux/iosys-map.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_plane.h>

struct drm_simple_display_pipe;

/*
 * Plane Helpers
 */

int drm_gem_plane_helper_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state);

/*
 * Helpers for planes with shadow buffers
 */

/**
 * DRM_SHADOW_PLANE_MAX_WIDTH - Maximum width of a plane's shadow buffer in pixels
 *
 * For drivers with shadow planes, the maximum width of the framebuffer is
 * usually independent from hardware limitations. Drivers can initialize struct
 * drm_mode_config.max_width from DRM_SHADOW_PLANE_MAX_WIDTH.
 */
#define DRM_SHADOW_PLANE_MAX_WIDTH	(4096u)

/**
 * DRM_SHADOW_PLANE_MAX_HEIGHT - Maximum height of a plane's shadow buffer in scanlines
 *
 * For drivers with shadow planes, the maximum height of the framebuffer is
 * usually independent from hardware limitations. Drivers can initialize struct
 * drm_mode_config.max_height from DRM_SHADOW_PLANE_MAX_HEIGHT.
 */
#define DRM_SHADOW_PLANE_MAX_HEIGHT	(4096u)

/**
 * struct drm_shadow_plane_state - plane state for planes with shadow buffers
 *
 * For planes that use a shadow buffer, struct drm_shadow_plane_state
 * provides the regular plane state plus mappings of the shadow buffer
 * into kernel address space.
 */
struct drm_shadow_plane_state {
	/** @base: plane state */
	struct drm_plane_state base;

	/* Transitional state - do not export or duplicate */

	/**
	 * @map: Mappings of the plane's framebuffer BOs in to kernel address space
	 *
	 * The memory mappings stored in map should be established in the plane's
	 * prepare_fb callback and removed in the cleanup_fb callback.
	 */
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];

	/**
	 * @data: Address of each framebuffer BO's data
	 *
	 * The address of the data stored in each mapping. This is different
	 * for framebuffers with non-zero offset fields.
	 */
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
};

/**
 * to_drm_shadow_plane_state - upcasts from struct drm_plane_state
 * @state: the plane state
 */
static inline struct drm_shadow_plane_state *
to_drm_shadow_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct drm_shadow_plane_state, base);
}

void __drm_gem_duplicate_shadow_plane_state(struct drm_plane *plane,
					    struct drm_shadow_plane_state *new_shadow_plane_state);
void __drm_gem_destroy_shadow_plane_state(struct drm_shadow_plane_state *shadow_plane_state);
void __drm_gem_reset_shadow_plane(struct drm_plane *plane,
				  struct drm_shadow_plane_state *shadow_plane_state);

void drm_gem_reset_shadow_plane(struct drm_plane *plane);
struct drm_plane_state *drm_gem_duplicate_shadow_plane_state(struct drm_plane *plane);
void drm_gem_destroy_shadow_plane_state(struct drm_plane *plane,
					struct drm_plane_state *plane_state);

/**
 * DRM_GEM_SHADOW_PLANE_FUNCS -
 *	Initializes struct drm_plane_funcs for shadow-buffered planes
 *
 * Drivers may use GEM BOs as shadow buffers over the framebuffer memory. This
 * macro initializes struct drm_plane_funcs to use the rsp helper functions.
 */
#define DRM_GEM_SHADOW_PLANE_FUNCS \
	.reset = drm_gem_reset_shadow_plane, \
	.atomic_duplicate_state = drm_gem_duplicate_shadow_plane_state, \
	.atomic_destroy_state = drm_gem_destroy_shadow_plane_state

int drm_gem_begin_shadow_fb_access(struct drm_plane *plane, struct drm_plane_state *plane_state);
void drm_gem_end_shadow_fb_access(struct drm_plane *plane, struct drm_plane_state *plane_state);

/**
 * DRM_GEM_SHADOW_PLANE_HELPER_FUNCS -
 *	Initializes struct drm_plane_helper_funcs for shadow-buffered planes
 *
 * Drivers may use GEM BOs as shadow buffers over the framebuffer memory. This
 * macro initializes struct drm_plane_helper_funcs to use the rsp helper
 * functions.
 */
#define DRM_GEM_SHADOW_PLANE_HELPER_FUNCS \
	.begin_fb_access = drm_gem_begin_shadow_fb_access, \
	.end_fb_access = drm_gem_end_shadow_fb_access

int drm_gem_simple_kms_begin_shadow_fb_access(struct drm_simple_display_pipe *pipe,
					      struct drm_plane_state *plane_state);
void drm_gem_simple_kms_end_shadow_fb_access(struct drm_simple_display_pipe *pipe,
					     struct drm_plane_state *plane_state);
void drm_gem_simple_kms_reset_shadow_plane(struct drm_simple_display_pipe *pipe);
struct drm_plane_state *
drm_gem_simple_kms_duplicate_shadow_plane_state(struct drm_simple_display_pipe *pipe);
void drm_gem_simple_kms_destroy_shadow_plane_state(struct drm_simple_display_pipe *pipe,
						   struct drm_plane_state *plane_state);

/**
 * DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS -
 *	Initializes struct drm_simple_display_pipe_funcs for shadow-buffered planes
 *
 * Drivers may use GEM BOs as shadow buffers over the framebuffer memory. This
 * macro initializes struct drm_simple_display_pipe_funcs to use the rsp helper
 * functions.
 */
#define DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS \
	.begin_fb_access = drm_gem_simple_kms_begin_shadow_fb_access, \
	.end_fb_access = drm_gem_simple_kms_end_shadow_fb_access, \
	.reset_plane = drm_gem_simple_kms_reset_shadow_plane, \
	.duplicate_plane_state = drm_gem_simple_kms_duplicate_shadow_plane_state, \
	.destroy_plane_state = drm_gem_simple_kms_destroy_shadow_plane_state

#endif /* __DRM_GEM_ATOMIC_HELPER_H__ */
