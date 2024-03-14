// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 Linaro Limited
 * Copyright 2023 Intel Corporation
 *
 * Library routines for populating a generic thermal trip point structure
 * with data obtained by evaluating a specific object in the ACPI Namespace.
 */
#include <linux/acpi.h>
#include <linux/units.h>

#include "thermal_core.h"

/*
 * Minimum temperature for full military grade is 218°K (-55°C) and
 * max temperature is 448°K (175°C). We can consider those values as
 * the boundaries for the [trips] temperature returned by the
 * firmware. Any values out of these boundaries may be considered
 * bogus and we can assume the firmware has no data to provide.
 */
#define TEMP_MIN_DECIK	2180
#define TEMP_MAX_DECIK	4480

static int thermal_acpi_trip_temp(struct acpi_device *adev, char *obj_name,
				  int *ret_temp)
{
	unsigned long long temp;
	acpi_status status;

	status = acpi_evaluate_integer(adev->handle, obj_name, NULL, &temp);
	if (ACPI_FAILURE(status)) {
		acpi_handle_debug(adev->handle, "%s evaluation failed\n", obj_name);
		return -ENODATA;
	}

	if (temp >= TEMP_MIN_DECIK && temp <= TEMP_MAX_DECIK) {
		*ret_temp = deci_kelvin_to_millicelsius(temp);
	} else {
		acpi_handle_debug(adev->handle, "%s result %llu out of range\n",
				  obj_name, temp);
		*ret_temp = THERMAL_TEMP_INVALID;
	}

	return 0;
}

/**
 * thermal_acpi_active_trip_temp - Retrieve active trip point temperature
 * @adev: Target thermal zone ACPI device object.
 * @id: Active cooling level (0 - 9).
 * @ret_temp: Address to store the retrieved temperature value on success.
 *
 * Evaluate the _ACx object for the thermal zone represented by @adev to obtain
 * the temperature of the active cooling trip point corresponding to the active
 * cooling level given by @id.
 *
 * Return 0 on success or a negative error value on failure.
 */
int thermal_acpi_active_trip_temp(struct acpi_device *adev, int id, int *ret_temp)
{
	char obj_name[] = {'_', 'A', 'C', '0' + id, '\0'};

	if (id < 0 || id > 9)
		return -EINVAL;

	return thermal_acpi_trip_temp(adev, obj_name, ret_temp);
}
EXPORT_SYMBOL_GPL(thermal_acpi_active_trip_temp);

/**
 * thermal_acpi_passive_trip_temp - Retrieve passive trip point temperature
 * @adev: Target thermal zone ACPI device object.
 * @ret_temp: Address to store the retrieved temperature value on success.
 *
 * Evaluate the _PSV object for the thermal zone represented by @adev to obtain
 * the temperature of the passive cooling trip point.
 *
 * Return 0 on success or -ENODATA on failure.
 */
int thermal_acpi_passive_trip_temp(struct acpi_device *adev, int *ret_temp)
{
	return thermal_acpi_trip_temp(adev, "_PSV", ret_temp);
}
EXPORT_SYMBOL_GPL(thermal_acpi_passive_trip_temp);

/**
 * thermal_acpi_hot_trip_temp - Retrieve hot trip point temperature
 * @adev: Target thermal zone ACPI device object.
 * @ret_temp: Address to store the retrieved temperature value on success.
 *
 * Evaluate the _HOT object for the thermal zone represented by @adev to obtain
 * the temperature of the trip point at which the system is expected to be put
 * into the S4 sleep state.
 *
 * Return 0 on success or -ENODATA on failure.
 */
int thermal_acpi_hot_trip_temp(struct acpi_device *adev, int *ret_temp)
{
	return thermal_acpi_trip_temp(adev, "_HOT", ret_temp);
}
EXPORT_SYMBOL_GPL(thermal_acpi_hot_trip_temp);

/**
 * thermal_acpi_critical_trip_temp - Retrieve critical trip point temperature
 * @adev: Target thermal zone ACPI device object.
 * @ret_temp: Address to store the retrieved temperature value on success.
 *
 * Evaluate the _CRT object for the thermal zone represented by @adev to obtain
 * the temperature of the critical cooling trip point.
 *
 * Return 0 on success or -ENODATA on failure.
 */
int thermal_acpi_critical_trip_temp(struct acpi_device *adev, int *ret_temp)
{
	return thermal_acpi_trip_temp(adev, "_CRT", ret_temp);
}
EXPORT_SYMBOL_GPL(thermal_acpi_critical_trip_temp);
