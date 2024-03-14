// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2023 Advanced Micro Devices, Inc.
//
// Authors: Venkata Prasad Potturu <venkataprasad.potturu@amd.com>

/*
 * Hardware interface for Audio DSP on Vangogh platform
 */

#include <linux/platform_device.h>
#include <linux/module.h>

#include "../ops.h"
#include "../sof-audio.h"
#include "acp.h"
#include "acp-dsp-offset.h"

#define I2S_HS_INSTANCE		0
#define I2S_BT_INSTANCE		1
#define I2S_SP_INSTANCE		2
#define PDM_DMIC_INSTANCE	3
#define I2S_HS_VIRTUAL_INSTANCE	4

static struct snd_soc_dai_driver vangogh_sof_dai[] = {
	[I2S_HS_INSTANCE] = {
		.id = I2S_HS_INSTANCE,
		.name = "acp-sof-hs",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S HS controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_BT_INSTANCE] = {
		.id = I2S_BT_INSTANCE,
		.name = "acp-sof-bt",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S BT controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_SP_INSTANCE] = {
		.id = I2S_SP_INSTANCE,
		.name = "acp-sof-sp",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S SP controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[PDM_DMIC_INSTANCE] = {
		.id = PDM_DMIC_INSTANCE,
		.name = "acp-sof-dmic",
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 4,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_HS_VIRTUAL_INSTANCE] = {
		.id = I2S_HS_VIRTUAL_INSTANCE,
		.name = "acp-sof-hs-virtual",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
					   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S HS-Virtual controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},
};

/* Vangogh ops */
struct snd_sof_dsp_ops sof_vangogh_ops;
EXPORT_SYMBOL_NS(sof_vangogh_ops, SND_SOC_SOF_AMD_COMMON);

int sof_vangogh_ops_init(struct snd_sof_dev *sdev)
{
	const struct dmi_system_id *dmi_id;

	/* common defaults */
	memcpy(&sof_vangogh_ops, &sof_acp_common_ops, sizeof(struct snd_sof_dsp_ops));

	sof_vangogh_ops.drv = vangogh_sof_dai;
	sof_vangogh_ops.num_drv = ARRAY_SIZE(vangogh_sof_dai);

	dmi_id = dmi_first_match(acp_sof_quirk_table);
	if (dmi_id && dmi_id->driver_data)
		sof_vangogh_ops.load_firmware = acp_sof_load_signed_firmware;

	return 0;
}

MODULE_IMPORT_NS(SND_SOC_SOF_AMD_COMMON);
MODULE_DESCRIPTION("VANGOGH SOF Driver");
MODULE_LICENSE("Dual BSD/GPL");
