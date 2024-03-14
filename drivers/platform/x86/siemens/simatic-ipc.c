// SPDX-License-Identifier: GPL-2.0
/*
 * Siemens SIMATIC IPC platform driver
 *
 * Copyright (c) Siemens AG, 2018-2023
 *
 * Authors:
 *  Henning Schild <henning.schild@siemens.com>
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *  Gerd Haeussler <gerd.haeussler.ext@siemens.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dmi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_data/x86/simatic-ipc.h>
#include <linux/platform_device.h>

static struct platform_device *ipc_led_platform_device;
static struct platform_device *ipc_wdt_platform_device;
static struct platform_device *ipc_batt_platform_device;

static const struct dmi_system_id simatic_ipc_whitelist[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SIEMENS AG"),
		},
	},
	{}
};

static struct simatic_ipc_platform platform_data;

#define SIMATIC_IPC_MAX_EXTRA_MODULES 2

static struct {
	u32 station_id;
	u8 led_mode;
	u8 wdt_mode;
	u8 batt_mode;
	char *extra_modules[SIMATIC_IPC_MAX_EXTRA_MODULES];
} device_modes[] = {
	{SIMATIC_IPC_IPC127E,
		SIMATIC_IPC_DEVICE_127E, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_127E,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC227D,
		SIMATIC_IPC_DEVICE_227D, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_NONE,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC227E,
		SIMATIC_IPC_DEVICE_427E, SIMATIC_IPC_DEVICE_227E, SIMATIC_IPC_DEVICE_227E,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC227G,
		SIMATIC_IPC_DEVICE_227G, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_227G,
		{ "nct6775", "w83627hf_wdt" }},
	{SIMATIC_IPC_IPC277G,
		SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_227G,
		{ "nct6775", "w83627hf_wdt" }},
	{SIMATIC_IPC_IPC277E,
		SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_227E, SIMATIC_IPC_DEVICE_227E,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC427D,
		SIMATIC_IPC_DEVICE_427E, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_NONE,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC427E,
		SIMATIC_IPC_DEVICE_427E, SIMATIC_IPC_DEVICE_427E, SIMATIC_IPC_DEVICE_NONE,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPC477E,
		SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_427E, SIMATIC_IPC_DEVICE_NONE,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPCBX_39A,
		SIMATIC_IPC_DEVICE_227G, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_BX_39A,
		{ "nct6775", "w83627hf_wdt" }},
	{SIMATIC_IPC_IPCPX_39A,
		SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_BX_39A,
		{ "nct6775", "w83627hf_wdt" }},
	{SIMATIC_IPC_IPCBX_21A,
		SIMATIC_IPC_DEVICE_BX_21A, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_BX_21A,
		{ "emc1403", NULL }},
	{SIMATIC_IPC_IPCBX_56A,
		SIMATIC_IPC_DEVICE_BX_59A, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_BX_59A,
		{ "emc1403", "w83627hf_wdt" }},
	{SIMATIC_IPC_IPCBX_59A,
		SIMATIC_IPC_DEVICE_BX_59A, SIMATIC_IPC_DEVICE_NONE, SIMATIC_IPC_DEVICE_BX_59A,
		{ "emc1403", "w83627hf_wdt" }},
};

static int register_platform_devices(u32 station_id)
{
	u8 ledmode = SIMATIC_IPC_DEVICE_NONE;
	u8 wdtmode = SIMATIC_IPC_DEVICE_NONE;
	u8 battmode = SIMATIC_IPC_DEVICE_NONE;
	char *pdevname;
	int i;

	for (i = 0; i < ARRAY_SIZE(device_modes); i++) {
		if (device_modes[i].station_id == station_id) {
			ledmode = device_modes[i].led_mode;
			wdtmode = device_modes[i].wdt_mode;
			battmode = device_modes[i].batt_mode;
			break;
		}
	}

	if (battmode != SIMATIC_IPC_DEVICE_NONE) {
		pdevname = KBUILD_MODNAME "_batt";
		if (battmode == SIMATIC_IPC_DEVICE_127E)
			pdevname = KBUILD_MODNAME "_batt_apollolake";
		if (battmode == SIMATIC_IPC_DEVICE_BX_21A)
			pdevname = KBUILD_MODNAME "_batt_elkhartlake";
		if (battmode == SIMATIC_IPC_DEVICE_227G ||
		    battmode == SIMATIC_IPC_DEVICE_BX_39A ||
		    battmode == SIMATIC_IPC_DEVICE_BX_59A)
			pdevname = KBUILD_MODNAME "_batt_f7188x";
		platform_data.devmode = battmode;
		ipc_batt_platform_device =
			platform_device_register_data(NULL, pdevname,
				PLATFORM_DEVID_NONE, &platform_data,
				sizeof(struct simatic_ipc_platform));
		if (IS_ERR(ipc_batt_platform_device))
			return PTR_ERR(ipc_batt_platform_device);

		pr_debug("device=%s created\n",
			 ipc_batt_platform_device->name);
	}

	if (ledmode != SIMATIC_IPC_DEVICE_NONE) {
		pdevname = KBUILD_MODNAME "_leds";
		if (ledmode == SIMATIC_IPC_DEVICE_127E)
			pdevname = KBUILD_MODNAME "_leds_gpio_apollolake";
		if (ledmode == SIMATIC_IPC_DEVICE_227G || ledmode == SIMATIC_IPC_DEVICE_BX_59A)
			pdevname = KBUILD_MODNAME "_leds_gpio_f7188x";
		if (ledmode == SIMATIC_IPC_DEVICE_BX_21A)
			pdevname = KBUILD_MODNAME "_leds_gpio_elkhartlake";
		platform_data.devmode = ledmode;
		ipc_led_platform_device =
			platform_device_register_data(NULL,
				pdevname, PLATFORM_DEVID_NONE,
				&platform_data,
				sizeof(struct simatic_ipc_platform));
		if (IS_ERR(ipc_led_platform_device))
			return PTR_ERR(ipc_led_platform_device);

		pr_debug("device=%s created\n",
			 ipc_led_platform_device->name);
	}

	if (wdtmode != SIMATIC_IPC_DEVICE_NONE) {
		platform_data.devmode = wdtmode;
		ipc_wdt_platform_device =
			platform_device_register_data(NULL,
				KBUILD_MODNAME "_wdt", PLATFORM_DEVID_NONE,
				&platform_data,
				sizeof(struct simatic_ipc_platform));
		if (IS_ERR(ipc_wdt_platform_device))
			return PTR_ERR(ipc_wdt_platform_device);

		pr_debug("device=%s created\n",
			 ipc_wdt_platform_device->name);
	}

	if (ledmode == SIMATIC_IPC_DEVICE_NONE &&
	    wdtmode == SIMATIC_IPC_DEVICE_NONE &&
	    battmode == SIMATIC_IPC_DEVICE_NONE) {
		pr_warn("unsupported IPC detected, station id=%08x\n",
			station_id);
		return -EINVAL;
	}

	return 0;
}

static void request_additional_modules(u32 station_id)
{
	char **extra_modules = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(device_modes); i++) {
		if (device_modes[i].station_id == station_id) {
			extra_modules = device_modes[i].extra_modules;
			break;
		}
	}

	if (!extra_modules)
		return;

	for (i = 0; i < SIMATIC_IPC_MAX_EXTRA_MODULES; i++) {
		if (extra_modules[i])
			request_module(extra_modules[i]);
		else
			break;
	}
}

static int __init simatic_ipc_init_module(void)
{
	const struct dmi_system_id *match;
	u32 station_id;
	int err;

	match = dmi_first_match(simatic_ipc_whitelist);
	if (!match)
		return 0;

	err = dmi_walk(simatic_ipc_find_dmi_entry_helper, &station_id);

	if (err || station_id == SIMATIC_IPC_INVALID_STATION_ID) {
		pr_warn("DMI entry %d not found\n", SIMATIC_IPC_DMI_ENTRY_OEM);
		return 0;
	}

	request_additional_modules(station_id);

	return register_platform_devices(station_id);
}

static void __exit simatic_ipc_exit_module(void)
{
	platform_device_unregister(ipc_led_platform_device);
	ipc_led_platform_device = NULL;

	platform_device_unregister(ipc_wdt_platform_device);
	ipc_wdt_platform_device = NULL;

	platform_device_unregister(ipc_batt_platform_device);
	ipc_batt_platform_device = NULL;
}

module_init(simatic_ipc_init_module);
module_exit(simatic_ipc_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Gerd Haeussler <gerd.haeussler.ext@siemens.com>");
MODULE_ALIAS("dmi:*:svnSIEMENSAG:*");
