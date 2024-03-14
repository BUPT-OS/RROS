// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Advanced Micro Devices, Inc.
//
// Authors: Ajit Kumar Pandey <AjitKumar.Pandey@amd.com>
//

/* ACP machine configuration module */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "../sof/amd/acp.h"
#include "mach-config.h"

static int acp_quirk_data;

static const struct config_entry config_table[] = {
	{
		.flags = FLAG_AMD_SOF,
		.device = ACP_PCI_DEV_ID,
		.dmi_table = (const struct dmi_system_id []) {
			{
				.matches = {
					DMI_MATCH(DMI_SYS_VENDOR, "AMD"),
					DMI_MATCH(DMI_PRODUCT_NAME, "Majolica-CZN"),
				},
			},
			{}
		},
	},
	{
		.flags = FLAG_AMD_SOF,
		.device = ACP_PCI_DEV_ID,
		.dmi_table = (const struct dmi_system_id []) {
			{
				.matches = {
					DMI_MATCH(DMI_SYS_VENDOR, "Google"),
				},
			},
			{}
		},
	},
	{
		.flags = FLAG_AMD_SOF,
		.device = ACP_PCI_DEV_ID,
		.dmi_table = (const struct dmi_system_id []) {
			{
				.matches = {
					DMI_MATCH(DMI_SYS_VENDOR, "Valve"),
					DMI_MATCH(DMI_PRODUCT_NAME, "Galileo"),
					DMI_MATCH(DMI_PRODUCT_FAMILY, "Sephiroth"),
				},
			},
			{}
		},
	},
};

int snd_amd_acp_find_config(struct pci_dev *pci)
{
	const struct config_entry *table = config_table;
	u16 device = pci->device;
	int i;

	/* Do not enable FLAGS on older platforms with Rev id zero */
	if (!pci->revision)
		return 0;

	for (i = 0; i < ARRAY_SIZE(config_table); i++, table++) {
		if (table->device != device)
			continue;
		if (table->dmi_table && !dmi_check_system(table->dmi_table))
			continue;
		acp_quirk_data = table->flags;
		return table->flags;
	}

	return 0;
}
EXPORT_SYMBOL(snd_amd_acp_find_config);

static struct snd_soc_acpi_codecs amp_rt1019 = {
	.num_codecs = 1,
	.codecs = {"10EC1019"}
};

static struct snd_soc_acpi_codecs amp_max = {
	.num_codecs = 1,
	.codecs = {"MX98360A"}
};

static struct snd_soc_acpi_codecs amp_max98388 = {
	.num_codecs = 1,
	.codecs = {"ADS8388"}
};

struct snd_soc_acpi_mach snd_soc_acpi_amd_sof_machines[] = {
	{
		.id = "10EC5682",
		.drv_name = "rt5682-rt1019",
		.pdata = (void *)&acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_rt1019,
		.fw_filename = "sof-rn.ri",
		.sof_tplg_filename = "sof-rn-rt5682-rt1019.tplg",
	},
	{
		.id = "10EC5682",
		.drv_name = "rt5682-max",
		.pdata = (void *)&acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_max,
		.fw_filename = "sof-rn.ri",
		.sof_tplg_filename = "sof-rn-rt5682-max98360.tplg",
	},
	{
		.id = "RTL5682",
		.drv_name = "rt5682s-max",
		.pdata = (void *)&acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_max,
		.fw_filename = "sof-rn.ri",
		.sof_tplg_filename = "sof-rn-rt5682-max98360.tplg",
	},
	{
		.id = "RTL5682",
		.drv_name = "rt5682s-rt1019",
		.pdata = (void *)&acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_rt1019,
		.fw_filename = "sof-rn.ri",
		.sof_tplg_filename = "sof-rn-rt5682-rt1019.tplg",
	},
	{
		.id = "AMDI1019",
		.drv_name = "renoir-dsp",
		.pdata = (void *)&acp_quirk_data,
		.fw_filename = "sof-rn.ri",
		.sof_tplg_filename = "sof-acp.tplg",
	},
	{},
};
EXPORT_SYMBOL(snd_soc_acpi_amd_sof_machines);

struct snd_soc_acpi_mach snd_soc_acpi_amd_vangogh_sof_machines[] = {
	{
		.id = "NVTN2020",
		.drv_name = "nau8821-max",
		.pdata = &acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_max98388,
		.fw_filename = "sof-vangogh.ri",
		.sof_tplg_filename = "sof-vangogh-nau8821-max.tplg",
	},
	{},
};
EXPORT_SYMBOL(snd_soc_acpi_amd_vangogh_sof_machines);

struct snd_soc_acpi_mach snd_soc_acpi_amd_rmb_sof_machines[] = {
	{
		.id = "AMDI1019",
		.drv_name = "rmb-dsp",
		.pdata = &acp_quirk_data,
		.fw_filename = "sof-rmb.ri",
		.sof_tplg_filename = "sof-acp-rmb.tplg",
	},
	{
		.id = "10508825",
		.drv_name = "nau8825-max",
		.pdata = &acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_max,
		.fw_filename = "sof-rmb.ri",
		.sof_tplg_filename = "sof-rmb-nau8825-max98360.tplg",
	},
	{
		.id = "RTL5682",
		.drv_name = "rt5682s-hs-rt1019",
		.pdata = &acp_quirk_data,
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &amp_rt1019,
		.fw_filename = "sof-rmb.ri",
		.sof_tplg_filename = "sof-rmb-rt5682s-rt1019.tplg",
	},
	{},
};
EXPORT_SYMBOL(snd_soc_acpi_amd_rmb_sof_machines);

MODULE_LICENSE("Dual BSD/GPL");
