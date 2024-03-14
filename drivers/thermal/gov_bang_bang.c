// SPDX-License-Identifier: GPL-2.0-only
/*
 *  gov_bang_bang.c - A simple thermal throttling governor using hysteresis
 *
 *  Copyright (C) 2014 Peter Kaestle <peter@piie.net>
 *
 *  Based on step_wise.c with following Copyrights:
 *  Copyright (C) 2012 Intel Corp
 *  Copyright (C) 2012 Durgadoss R <durgadoss.r@intel.com>
 */

#include <linux/thermal.h>

#include "thermal_core.h"

static int thermal_zone_trip_update(struct thermal_zone_device *tz, int trip_id)
{
	struct thermal_trip trip;
	struct thermal_instance *instance;
	int ret;

	ret = __thermal_zone_get_trip(tz, trip_id, &trip);
	if (ret) {
		pr_warn_once("Failed to retrieve trip point %d\n", trip_id);
		return ret;
	}

	if (!trip.hysteresis)
		dev_info_once(&tz->device,
			      "Zero hysteresis value for thermal zone %s\n", tz->type);

	dev_dbg(&tz->device, "Trip%d[temp=%d]:temp=%d:hyst=%d\n",
				trip_id, trip.temperature, tz->temperature,
				trip.hysteresis);

	list_for_each_entry(instance, &tz->thermal_instances, tz_node) {
		if (instance->trip != trip_id)
			continue;

		/* in case fan is in initial state, switch the fan off */
		if (instance->target == THERMAL_NO_TARGET)
			instance->target = 0;

		/* in case fan is neither on nor off set the fan to active */
		if (instance->target != 0 && instance->target != 1) {
			pr_warn("Thermal instance %s controlled by bang-bang has unexpected state: %ld\n",
					instance->name, instance->target);
			instance->target = 1;
		}

		/*
		 * enable fan when temperature exceeds trip_temp and disable
		 * the fan in case it falls below trip_temp minus hysteresis
		 */
		if (instance->target == 0 && tz->temperature >= trip.temperature)
			instance->target = 1;
		else if (instance->target == 1 &&
			 tz->temperature <= trip.temperature - trip.hysteresis)
			instance->target = 0;

		dev_dbg(&instance->cdev->device, "target=%d\n",
					(int)instance->target);

		mutex_lock(&instance->cdev->lock);
		instance->cdev->updated = false; /* cdev needs update */
		mutex_unlock(&instance->cdev->lock);
	}

	return 0;
}

/**
 * bang_bang_control - controls devices associated with the given zone
 * @tz: thermal_zone_device
 * @trip: the trip point
 *
 * Regulation Logic: a two point regulation, deliver cooling state depending
 * on the previous state shown in this diagram:
 *
 *                Fan:   OFF    ON
 *
 *                              |
 *                              |
 *          trip_temp:    +---->+
 *                        |     |        ^
 *                        |     |        |
 *                        |     |   Temperature
 * (trip_temp - hyst):    +<----+
 *                        |
 *                        |
 *                        |
 *
 *   * If the fan is not running and temperature exceeds trip_temp, the fan
 *     gets turned on.
 *   * In case the fan is running, temperature must fall below
 *     (trip_temp - hyst) so that the fan gets turned off again.
 *
 */
static int bang_bang_control(struct thermal_zone_device *tz, int trip)
{
	struct thermal_instance *instance;
	int ret;

	lockdep_assert_held(&tz->lock);

	ret = thermal_zone_trip_update(tz, trip);
	if (ret)
		return ret;

	list_for_each_entry(instance, &tz->thermal_instances, tz_node)
		thermal_cdev_update(instance->cdev);

	return 0;
}

static struct thermal_governor thermal_gov_bang_bang = {
	.name		= "bang_bang",
	.throttle	= bang_bang_control,
};
THERMAL_GOVERNOR_DECLARE(thermal_gov_bang_bang);
