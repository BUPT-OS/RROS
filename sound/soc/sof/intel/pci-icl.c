// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018-2021 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

#include <linux/module.h>
#include <linux/pci.h>
#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include <sound/sof.h>
#include "../ops.h"
#include "../sof-pci-dev.h"

/* platform specific devices */
#include "hda.h"

static const struct sof_dev_desc icl_desc = {
	.machines               = snd_soc_acpi_intel_icl_machines,
	.alt_machines		= snd_soc_acpi_intel_icl_sdw_machines,
	.use_acpi_target_states	= true,
	.resindex_lpe_base      = 0,
	.resindex_pcicfg_base   = -1,
	.resindex_imr_base      = -1,
	.irqindex_host_ipc      = -1,
	.chip_info = &icl_chip_info,
	.ipc_supported_mask	= BIT(SOF_IPC) | BIT(SOF_INTEL_IPC4),
	.ipc_default		= SOF_IPC,
	.dspless_mode_supported	= true,		/* Only supported for HDaudio */
	.default_fw_path = {
		[SOF_IPC] = "intel/sof",
		[SOF_INTEL_IPC4] = "intel/avs/icl",
	},
	.default_lib_path = {
		[SOF_INTEL_IPC4] = "intel/avs-lib/icl",
	},
	.default_tplg_path = {
		[SOF_IPC] = "intel/sof-tplg",
		[SOF_INTEL_IPC4] = "intel/avs-tplg",
	},
	.default_fw_filename = {
		[SOF_IPC] = "sof-icl.ri",
		[SOF_INTEL_IPC4] = "dsp_basefw.bin",
	},
	.nocodec_tplg_filename = "sof-icl-nocodec.tplg",
	.ops = &sof_icl_ops,
	.ops_init = sof_icl_ops_init,
	.ops_free = hda_ops_free,
};

static const struct sof_dev_desc jsl_desc = {
	.machines               = snd_soc_acpi_intel_jsl_machines,
	.use_acpi_target_states	= true,
	.resindex_lpe_base      = 0,
	.resindex_pcicfg_base   = -1,
	.resindex_imr_base      = -1,
	.irqindex_host_ipc      = -1,
	.chip_info = &jsl_chip_info,
	.ipc_supported_mask	= BIT(SOF_IPC) | BIT(SOF_INTEL_IPC4),
	.ipc_default		= SOF_IPC,
	.dspless_mode_supported	= true,		/* Only supported for HDaudio */
	.default_fw_path = {
		[SOF_IPC] = "intel/sof",
		[SOF_INTEL_IPC4] = "intel/avs/jsl",
	},
	.default_lib_path = {
		[SOF_INTEL_IPC4] = "intel/avs-lib/jsl",
	},
	.default_tplg_path = {
		[SOF_IPC] = "intel/sof-tplg",
		[SOF_INTEL_IPC4] = "intel/avs-tplg",
	},
	.default_fw_filename = {
		[SOF_IPC] = "sof-jsl.ri",
		[SOF_INTEL_IPC4] = "dsp_basefw.bin",
	},
	.nocodec_tplg_filename = "sof-jsl-nocodec.tplg",
	.ops = &sof_cnl_ops,
	.ops_init = sof_cnl_ops_init,
	.ops_free = hda_ops_free,
};

/* PCI IDs */
static const struct pci_device_id sof_pci_ids[] = {
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_LP, &icl_desc) },
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_H, &icl_desc) },
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_N, &jsl_desc) },
	{ PCI_DEVICE_DATA(INTEL, HDA_JSL_N, &jsl_desc) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, sof_pci_ids);

/* pci_driver definition */
static struct pci_driver snd_sof_pci_intel_icl_driver = {
	.name = "sof-audio-pci-intel-icl",
	.id_table = sof_pci_ids,
	.probe = hda_pci_intel_probe,
	.remove = sof_pci_remove,
	.shutdown = sof_pci_shutdown,
	.driver = {
		.pm = &sof_pci_pm,
	},
};
module_pci_driver(snd_sof_pci_intel_icl_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_INTEL_HDA_COMMON);
MODULE_IMPORT_NS(SND_SOC_SOF_PCI_DEV);
