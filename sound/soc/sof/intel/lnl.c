// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.

/*
 * Hardware interface for audio DSP on LunarLake.
 */

#include <linux/firmware.h>
#include <sound/hda_register.h>
#include <sound/sof/ipc4/header.h>
#include <trace/events/sof_intel.h>
#include "../ipc4-priv.h"
#include "../ops.h"
#include "hda.h"
#include "hda-ipc.h"
#include "../sof-audio.h"
#include "mtl.h"
#include <sound/hda-mlink.h>

/* LunarLake ops */
struct snd_sof_dsp_ops sof_lnl_ops;
EXPORT_SYMBOL_NS(sof_lnl_ops, SND_SOC_SOF_INTEL_HDA_COMMON);

static const struct snd_sof_debugfs_map lnl_dsp_debugfs[] = {
	{"hda", HDA_DSP_HDA_BAR, 0, 0x4000, SOF_DEBUGFS_ACCESS_ALWAYS},
	{"pp", HDA_DSP_PP_BAR,  0, 0x1000, SOF_DEBUGFS_ACCESS_ALWAYS},
	{"dsp", HDA_DSP_BAR,  0, 0x10000, SOF_DEBUGFS_ACCESS_ALWAYS},
};

/* this helps allows the DSP to setup DMIC/SSP */
static int hdac_bus_offload_dmic_ssp(struct hdac_bus *bus)
{
	int ret;

	ret = hdac_bus_eml_enable_offload(bus, true,  AZX_REG_ML_LEPTR_ID_INTEL_SSP, true);
	if (ret < 0)
		return ret;

	ret = hdac_bus_eml_enable_offload(bus, true,  AZX_REG_ML_LEPTR_ID_INTEL_DMIC, true);
	if (ret < 0)
		return ret;

	return 0;
}

static int lnl_hda_dsp_probe(struct snd_sof_dev *sdev)
{
	int ret;

	ret = hda_dsp_probe(sdev);
	if (ret < 0)
		return ret;

	return hdac_bus_offload_dmic_ssp(sof_to_bus(sdev));
}

static int lnl_hda_dsp_resume(struct snd_sof_dev *sdev)
{
	int ret;

	ret = hda_dsp_resume(sdev);
	if (ret < 0)
		return ret;

	return hdac_bus_offload_dmic_ssp(sof_to_bus(sdev));
}

static int lnl_hda_dsp_runtime_resume(struct snd_sof_dev *sdev)
{
	int ret;

	ret = hda_dsp_runtime_resume(sdev);
	if (ret < 0)
		return ret;

	return hdac_bus_offload_dmic_ssp(sof_to_bus(sdev));
}

int sof_lnl_ops_init(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data;

	/* common defaults */
	memcpy(&sof_lnl_ops, &sof_hda_common_ops, sizeof(struct snd_sof_dsp_ops));

	/* probe */
	sof_lnl_ops.probe = lnl_hda_dsp_probe;

	/* shutdown */
	sof_lnl_ops.shutdown = hda_dsp_shutdown;

	/* doorbell */
	sof_lnl_ops.irq_thread = mtl_ipc_irq_thread;

	/* ipc */
	sof_lnl_ops.send_msg = mtl_ipc_send_msg;
	sof_lnl_ops.get_mailbox_offset = mtl_dsp_ipc_get_mailbox_offset;
	sof_lnl_ops.get_window_offset = mtl_dsp_ipc_get_window_offset;

	/* debug */
	sof_lnl_ops.debug_map = lnl_dsp_debugfs;
	sof_lnl_ops.debug_map_count = ARRAY_SIZE(lnl_dsp_debugfs);
	sof_lnl_ops.dbg_dump = mtl_dsp_dump;
	sof_lnl_ops.ipc_dump = mtl_ipc_dump;

	/* pre/post fw run */
	sof_lnl_ops.pre_fw_run = mtl_dsp_pre_fw_run;
	sof_lnl_ops.post_fw_run = mtl_dsp_post_fw_run;

	/* parse platform specific extended manifest */
	sof_lnl_ops.parse_platform_ext_manifest = NULL;

	/* dsp core get/put */
	/* TODO: add core_get and core_put */

	/* PM */
	sof_lnl_ops.resume			= lnl_hda_dsp_resume;
	sof_lnl_ops.runtime_resume		= lnl_hda_dsp_runtime_resume;

	sof_lnl_ops.get_stream_position = mtl_dsp_get_stream_hda_link_position;

	sdev->private = devm_kzalloc(sdev->dev, sizeof(struct sof_ipc4_fw_data), GFP_KERNEL);
	if (!sdev->private)
		return -ENOMEM;

	ipc4_data = sdev->private;
	ipc4_data->manifest_fw_hdr_offset = SOF_MAN4_FW_HDR_OFFSET;

	ipc4_data->mtrace_type = SOF_IPC4_MTRACE_INTEL_CAVS_2;

	/* External library loading support */
	ipc4_data->load_library = hda_dsp_ipc4_load_library;

	/* set DAI ops */
	hda_set_dai_drv_ops(sdev, &sof_lnl_ops);

	sof_lnl_ops.set_power_state = hda_dsp_set_power_state_ipc4;

	return 0;
};
EXPORT_SYMBOL_NS(sof_lnl_ops_init, SND_SOC_SOF_INTEL_HDA_COMMON);

/* Check if an SDW IRQ occurred */
static bool lnl_dsp_check_sdw_irq(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);

	return hdac_bus_eml_check_interrupt(bus, true,  AZX_REG_ML_LEPTR_ID_SDW);
}

static void lnl_enable_sdw_irq(struct snd_sof_dev *sdev, bool enable)
{
	struct hdac_bus *bus = sof_to_bus(sdev);

	hdac_bus_eml_enable_interrupt(bus, true,  AZX_REG_ML_LEPTR_ID_SDW, enable);
}

static int lnl_dsp_disable_interrupts(struct snd_sof_dev *sdev)
{
	lnl_enable_sdw_irq(sdev, false);
	mtl_disable_ipc_interrupts(sdev);
	return mtl_enable_interrupts(sdev, false);
}

const struct sof_intel_dsp_desc lnl_chip_info = {
	.cores_num = 5,
	.init_core_mask = BIT(0),
	.host_managed_cores_mask = BIT(0),
	.ipc_req = MTL_DSP_REG_HFIPCXIDR,
	.ipc_req_mask = MTL_DSP_REG_HFIPCXIDR_BUSY,
	.ipc_ack = MTL_DSP_REG_HFIPCXIDA,
	.ipc_ack_mask = MTL_DSP_REG_HFIPCXIDA_DONE,
	.ipc_ctl = MTL_DSP_REG_HFIPCXCTL,
	.rom_status_reg = MTL_DSP_ROM_STS,
	.rom_init_timeout = 300,
	.ssp_count = MTL_SSP_COUNT,
	.d0i3_offset = MTL_HDA_VS_D0I3C,
	.read_sdw_lcount =  hda_sdw_check_lcount_ext,
	.enable_sdw_irq = lnl_enable_sdw_irq,
	.check_sdw_irq = lnl_dsp_check_sdw_irq,
	.check_ipc_irq = mtl_dsp_check_ipc_irq,
	.cl_init = mtl_dsp_cl_init,
	.power_down_dsp = mtl_power_down_dsp,
	.disable_interrupts = lnl_dsp_disable_interrupts,
	.hw_ip_version = SOF_INTEL_ACE_2_0,
};
EXPORT_SYMBOL_NS(lnl_chip_info, SND_SOC_SOF_INTEL_HDA_COMMON);
