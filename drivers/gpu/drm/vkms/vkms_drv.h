/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_DRV_H_
#define _VKMS_DRV_H_

#include <linux/hrtimer.h>

#include <drm/drm.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#define XRES_MIN    10
#define YRES_MIN    10

#define XRES_DEF  1024
#define YRES_DEF   768

#define XRES_MAX  8192
#define YRES_MAX  8192

#define NUM_OVERLAY_PLANES 8

#define VKMS_LUT_SIZE 256

struct vkms_frame_info {
	struct drm_framebuffer *fb;
	struct drm_rect src, dst;
	struct drm_rect rotated;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	unsigned int rotation;
	unsigned int offset;
	unsigned int pitch;
	unsigned int cpp;
};

struct pixel_argb_u16 {
	u16 a, r, g, b;
};

struct line_buffer {
	size_t n_pixels;
	struct pixel_argb_u16 *pixels;
};

struct vkms_writeback_job {
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
	struct vkms_frame_info wb_frame_info;
	void (*pixel_write)(u8 *dst_pixels, struct pixel_argb_u16 *in_pixel);
};

/**
 * vkms_plane_state - Driver specific plane state
 * @base: base plane state
 * @frame_info: data required for composing computation
 */
struct vkms_plane_state {
	struct drm_shadow_plane_state base;
	struct vkms_frame_info *frame_info;
	void (*pixel_read)(u8 *src_buffer, struct pixel_argb_u16 *out_pixel);
};

struct vkms_plane {
	struct drm_plane base;
};

struct vkms_color_lut {
	struct drm_color_lut *base;
	size_t lut_length;
	s64 channel_value2index_ratio;
};

/**
 * vkms_crtc_state - Driver specific CRTC state
 * @base: base CRTC state
 * @composer_work: work struct to compose and add CRC entries
 * @n_frame_start: start frame number for computed CRC
 * @n_frame_end: end frame number for computed CRC
 */
struct vkms_crtc_state {
	struct drm_crtc_state base;
	struct work_struct composer_work;

	int num_active_planes;
	/* stack of active planes for crc computation, should be in z order */
	struct vkms_plane_state **active_planes;
	struct vkms_writeback_job *active_writeback;
	struct vkms_color_lut gamma_lut;

	/* below four are protected by vkms_output.composer_lock */
	bool crc_pending;
	bool wb_pending;
	u64 frame_start;
	u64 frame_end;
};

struct vkms_output {
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_writeback_connector wb_connector;
	struct hrtimer vblank_hrtimer;
	ktime_t period_ns;
	struct drm_pending_vblank_event *event;
	/* ordered wq for composer_work */
	struct workqueue_struct *composer_workq;
	/* protects concurrent access to composer */
	spinlock_t lock;

	/* protected by @lock */
	bool composer_enabled;
	struct vkms_crtc_state *composer_state;

	spinlock_t composer_lock;
};

struct vkms_device;

struct vkms_config {
	bool writeback;
	bool cursor;
	bool overlay;
	/* only set when instantiated */
	struct vkms_device *dev;
};

struct vkms_device {
	struct drm_device drm;
	struct platform_device *platform;
	struct vkms_output output;
	const struct vkms_config *config;
};

#define drm_crtc_to_vkms_output(target) \
	container_of(target, struct vkms_output, crtc)

#define drm_device_to_vkms_device(target) \
	container_of(target, struct vkms_device, drm)

#define to_vkms_crtc_state(target)\
	container_of(target, struct vkms_crtc_state, base)

#define to_vkms_plane_state(target)\
	container_of(target, struct vkms_plane_state, base.base)

/* CRTC */
int vkms_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_plane *primary, struct drm_plane *cursor);

int vkms_output_init(struct vkms_device *vkmsdev, int index);

struct vkms_plane *vkms_plane_init(struct vkms_device *vkmsdev,
				   enum drm_plane_type type, int index);

/* CRC Support */
const char *const *vkms_get_crc_sources(struct drm_crtc *crtc,
					size_t *count);
int vkms_set_crc_source(struct drm_crtc *crtc, const char *src_name);
int vkms_verify_crc_source(struct drm_crtc *crtc, const char *source_name,
			   size_t *values_cnt);

/* Composer Support */
void vkms_composer_worker(struct work_struct *work);
void vkms_set_composer(struct vkms_output *out, bool enabled);
void vkms_compose_row(struct line_buffer *stage_buffer, struct vkms_plane_state *plane, int y);
void vkms_writeback_row(struct vkms_writeback_job *wb, const struct line_buffer *src_buffer, int y);

/* Writeback */
int vkms_enable_writeback_connector(struct vkms_device *vkmsdev);

#endif /* _VKMS_DRV_H_ */
