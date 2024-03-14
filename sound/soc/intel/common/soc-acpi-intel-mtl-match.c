// SPDX-License-Identifier: GPL-2.0-only
/*
 * soc-acpi-intel-mtl-match.c - tables and support for MTL ACPI enumeration.
 *
 * Copyright (c) 2022, Intel Corporation.
 *
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include "soc-acpi-intel-sdw-mockup-match.h"

static const struct snd_soc_acpi_codecs mtl_max98357a_amp = {
	.num_codecs = 1,
	.codecs = {"MX98357A"}
};

static const struct snd_soc_acpi_codecs mtl_max98360a_amp = {
	.num_codecs = 1,
	.codecs = {"MX98360A"}
};

static const struct snd_soc_acpi_codecs mtl_rt1019p_amp = {
	.num_codecs = 1,
	.codecs = {"RTL1019"}
};

static const struct snd_soc_acpi_codecs mtl_rt5682_rt5682s_hp = {
	.num_codecs = 2,
	.codecs = {"10EC5682", "RTL5682"},
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_mtl_machines[] = {
	{
		.comp_ids = &mtl_rt5682_rt5682s_hp,
		.drv_name = "mtl_mx98357_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &mtl_max98357a_amp,
		.sof_tplg_filename = "sof-mtl-max98357a-rt5682.tplg",
	},
	{
		.comp_ids = &mtl_rt5682_rt5682s_hp,
		.drv_name = "mtl_mx98360_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &mtl_max98360a_amp,
		.sof_tplg_filename = "sof-mtl-max98360a-rt5682.tplg",
	},
	{
		.comp_ids = &mtl_rt5682_rt5682s_hp,
		.drv_name = "mtl_rt1019_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &mtl_rt1019p_amp,
		.sof_tplg_filename = "sof-mtl-rt1019-rt5682.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_mtl_machines);

static const struct snd_soc_acpi_endpoint single_endpoint = {
	.num = 0,
	.aggregated = 0,
	.group_position = 0,
	.group_id = 0,
};

static const struct snd_soc_acpi_endpoint spk_l_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 0,
	.group_id = 1,
};

static const struct snd_soc_acpi_endpoint spk_r_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 1,
	.group_id = 1,
};

static const struct snd_soc_acpi_endpoint rt712_endpoints[] = {
	{
		.num = 0,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
	{
		.num = 1,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
};

static const struct snd_soc_acpi_adr_device rt711_sdca_0_adr[] = {
	{
		.adr = 0x000030025D071101ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt711"
	}
};

static const struct snd_soc_acpi_adr_device rt712_0_single_adr[] = {
	{
		.adr = 0x000030025D071201ull,
		.num_endpoints = ARRAY_SIZE(rt712_endpoints),
		.endpoints = rt712_endpoints,
		.name_prefix = "rt712"
	}
};

static const struct snd_soc_acpi_adr_device rt1712_3_single_adr[] = {
	{
		.adr = 0x000330025D171201ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt712-dmic"
	}
};

static const struct snd_soc_acpi_adr_device mx8373_0_adr[] = {
	{
		.adr = 0x000023019F837300ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "Left"
	},
	{
		.adr = 0x000027019F837300ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "Right"
	}
};

static const struct snd_soc_acpi_adr_device rt5682_2_adr[] = {
	{
		.adr = 0x000221025D568200ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt5682"
	}
};

static const struct snd_soc_acpi_adr_device rt1316_2_group1_adr[] = {
	{
		.adr = 0x000230025D131601ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "rt1316-1"
	}
};

static const struct snd_soc_acpi_adr_device rt1316_3_group1_adr[] = {
	{
		.adr = 0x000331025D131601ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "rt1316-2"
	}
};

static const struct snd_soc_acpi_adr_device rt1318_1_group1_adr[] = {
	{
		.adr = 0x000130025D131801ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "rt1318-1"
	}
};

static const struct snd_soc_acpi_adr_device rt1318_2_group1_adr[] = {
	{
		.adr = 0x000232025D131801ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "rt1318-2"
	}
};

static const struct snd_soc_acpi_adr_device rt714_0_adr[] = {
	{
		.adr = 0x000030025D071401ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt714"
	}
};

static const struct snd_soc_acpi_adr_device rt714_1_adr[] = {
	{
		.adr = 0x000130025D071401ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt714"
	}
};

static const struct snd_soc_acpi_link_adr mtl_712_only[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt712_0_single_adr),
		.adr_d = rt712_0_single_adr,
	},
	{
		.mask = BIT(3),
		.num_adr = ARRAY_SIZE(rt1712_3_single_adr),
		.adr_d = rt1712_3_single_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr rt5682_link2_max98373_link0[] = {
	/* Expected order: jack -> amp */
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(rt5682_2_adr),
		.adr_d = rt5682_2_adr,
	},
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(mx8373_0_adr),
		.adr_d = mx8373_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr mtl_rvp[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_sdca_0_adr),
		.adr_d = rt711_sdca_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr mtl_3_in_1_sdca[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_sdca_0_adr),
		.adr_d = rt711_sdca_0_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(rt1316_2_group1_adr),
		.adr_d = rt1316_2_group1_adr,
	},
	{
		.mask = BIT(3),
		.num_adr = ARRAY_SIZE(rt1316_3_group1_adr),
		.adr_d = rt1316_3_group1_adr,
	},
	{
		.mask = BIT(1),
		.num_adr = ARRAY_SIZE(rt714_1_adr),
		.adr_d = rt714_1_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr mtl_sdw_rt1318_l12_rt714_l0[] = {
	{
		.mask = BIT(1),
		.num_adr = ARRAY_SIZE(rt1318_1_group1_adr),
		.adr_d = rt1318_1_group1_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(rt1318_2_group1_adr),
		.adr_d = rt1318_2_group1_adr,
	},
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt714_0_adr),
		.adr_d = rt714_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_adr_device mx8363_2_adr[] = {
	{
		.adr = 0x000230019F836300ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "Left"
	},
	{
		.adr = 0x000231019F836300ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "Right"
	}
};

static const struct snd_soc_acpi_adr_device cs42l42_0_adr[] = {
	{
		.adr = 0x00001001FA424200ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "cs42l42"
	}
};

static const struct snd_soc_acpi_link_adr cs42l42_link0_max98363_link2[] = {
	/* Expected order: jack -> amp */
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(cs42l42_0_adr),
		.adr_d = cs42l42_0_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(mx8363_2_adr),
		.adr_d = mx8363_2_adr,
	},
	{}
};

/* this table is used when there is no I2S codec present */
struct snd_soc_acpi_mach snd_soc_acpi_intel_mtl_sdw_machines[] = {
	/* mockup tests need to be first */
	{
		.link_mask = GENMASK(3, 0),
		.links = sdw_mockup_headset_2amps_mic,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt711-rt1308-rt715.tplg",
	},
	{
		.link_mask = BIT(0) | BIT(1) | BIT(3),
		.links = sdw_mockup_headset_1amp_mic,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt711-rt1308-mono-rt715.tplg",
	},
	{
		.link_mask = GENMASK(2, 0),
		.links = sdw_mockup_mic_headset_1amp,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt715-rt711-rt1308-mono.tplg",
	},
	{
		.link_mask = BIT(3) | BIT(0),
		.links = mtl_712_only,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt712-l0-rt1712-l3.tplg",
	},
	{
		.link_mask = GENMASK(2, 0),
		.links = mtl_sdw_rt1318_l12_rt714_l0,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt1318-l12-rt714-l0.tplg"
	},
	{
		.link_mask = GENMASK(3, 0),
		.links = mtl_3_in_1_sdca,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt711-l0-rt1316-l23-rt714-l1.tplg",
	},
	{
		.link_mask = BIT(0),
		.links = mtl_rvp,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-rt711.tplg",
	},
	{
		.link_mask = BIT(0) | BIT(2),
		.links = rt5682_link2_max98373_link0,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-sdw-rt5682-l2-max98373-l0.tplg",
	},
	{
		.link_mask = BIT(0) | BIT(2),
		.links = cs42l42_link0_max98363_link2,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-mtl-sdw-cs42l42-l0-max98363-l2.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_mtl_sdw_machines);
