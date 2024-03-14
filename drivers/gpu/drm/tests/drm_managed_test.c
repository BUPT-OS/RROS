// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_drv.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_managed.h>

#include <kunit/resource.h>

#include <linux/device.h>

/* Ought to be enough for anybody */
#define TEST_TIMEOUT_MS	100

struct managed_test_priv {
	bool action_done;
	wait_queue_head_t action_wq;
};

static void drm_action(struct drm_device *drm, void *ptr)
{
	struct managed_test_priv *priv = ptr;

	priv->action_done = true;
	wake_up_interruptible(&priv->action_wq);
}

static void drm_test_managed_run_action(struct kunit *test)
{
	struct managed_test_priv *priv;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	init_waitqueue_head(&priv->action_wq);

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	drm = __drm_kunit_helper_alloc_drm_device(test, dev, sizeof(*drm), 0, DRIVER_MODESET);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm);

	ret = drmm_add_action_or_reset(drm, drm_action, priv);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_dev_register(drm, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_dev_unregister(drm);
	drm_kunit_helper_free_device(test, dev);

	ret = wait_event_interruptible_timeout(priv->action_wq, priv->action_done,
					       msecs_to_jiffies(TEST_TIMEOUT_MS));
	KUNIT_EXPECT_GT(test, ret, 0);
}

static struct kunit_case drm_managed_tests[] = {
	KUNIT_CASE(drm_test_managed_run_action),
	{}
};

static struct kunit_suite drm_managed_test_suite = {
	.name = "drm-test-managed",
	.test_cases = drm_managed_tests
};

kunit_test_suite(drm_managed_test_suite);

MODULE_AUTHOR("Maxime Ripard <maxime@cerno.tech>");
MODULE_LICENSE("GPL");
