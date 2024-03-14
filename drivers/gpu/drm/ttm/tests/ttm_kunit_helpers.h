/* SPDX-License-Identifier: GPL-2.0 AND MIT */
/*
 * Copyright © 2023 Intel Corporation
 */
#ifndef TTM_KUNIT_HELPERS_H
#define TTM_KUNIT_HELPERS_H

#include <drm/drm_drv.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_bo.h>

#include <drm/drm_kunit_helpers.h>
#include <kunit/test.h>

extern struct ttm_device_funcs ttm_dev_funcs;

struct ttm_test_devices {
	struct drm_device *drm;
	struct device *dev;
	struct ttm_device *ttm_dev;
};

/* Building blocks for test-specific init functions */
int ttm_device_kunit_init(struct ttm_test_devices *priv,
			  struct ttm_device *ttm,
			  bool use_dma_alloc,
			  bool use_dma32);
struct ttm_buffer_object *ttm_bo_kunit_init(struct kunit *test,
					    struct ttm_test_devices *devs,
					    size_t size);

struct ttm_test_devices *ttm_test_devices_basic(struct kunit *test);
struct ttm_test_devices *ttm_test_devices_all(struct kunit *test);

void ttm_test_devices_put(struct kunit *test, struct ttm_test_devices *devs);

/* Generic init/fini for tests that only need DRM/TTM devices */
int ttm_test_devices_init(struct kunit *test);
void ttm_test_devices_fini(struct kunit *test);

#endif // TTM_KUNIT_HELPERS_H
