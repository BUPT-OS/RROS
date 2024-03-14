// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/interconnect.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/interconnect/qcom,sdm845.h>

#include "bcm-voter.h"
#include "icc-rpmh.h"
#include "sdm845.h"

static struct qcom_icc_node qhm_a1noc_cfg = {
	.name = "qhm_a1noc_cfg",
	.id = SDM845_MASTER_A1NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_SERVICE_A1NOC },
};

static struct qcom_icc_node qhm_qup1 = {
	.name = "qhm_qup1",
	.id = SDM845_MASTER_BLSP_1,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node qhm_tsif = {
	.name = "qhm_tsif",
	.id = SDM845_MASTER_TSIF,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node xm_sdc2 = {
	.name = "xm_sdc2",
	.id = SDM845_MASTER_SDCC_2,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node xm_sdc4 = {
	.name = "xm_sdc4",
	.id = SDM845_MASTER_SDCC_4,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node xm_ufs_card = {
	.name = "xm_ufs_card",
	.id = SDM845_MASTER_UFS_CARD,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node xm_ufs_mem = {
	.name = "xm_ufs_mem",
	.id = SDM845_MASTER_UFS_MEM,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_node xm_pcie_0 = {
	.name = "xm_pcie_0",
	.id = SDM845_MASTER_PCIE_0,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_ANOC_PCIE_A1NOC_SNOC },
};

static struct qcom_icc_node qhm_a2noc_cfg = {
	.name = "qhm_a2noc_cfg",
	.id = SDM845_MASTER_A2NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_SERVICE_A2NOC },
};

static struct qcom_icc_node qhm_qdss_bam = {
	.name = "qhm_qdss_bam",
	.id = SDM845_MASTER_QDSS_BAM,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node qhm_qup2 = {
	.name = "qhm_qup2",
	.id = SDM845_MASTER_BLSP_2,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node qnm_cnoc = {
	.name = "qnm_cnoc",
	.id = SDM845_MASTER_CNOC_A2NOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node qxm_crypto = {
	.name = "qxm_crypto",
	.id = SDM845_MASTER_CRYPTO,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node qxm_ipa = {
	.name = "qxm_ipa",
	.id = SDM845_MASTER_IPA,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node xm_pcie3_1 = {
	.name = "xm_pcie3_1",
	.id = SDM845_MASTER_PCIE_1,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_ANOC_PCIE_SNOC },
};

static struct qcom_icc_node xm_qdss_etr = {
	.name = "xm_qdss_etr",
	.id = SDM845_MASTER_QDSS_ETR,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node xm_usb3_0 = {
	.name = "xm_usb3_0",
	.id = SDM845_MASTER_USB3_0,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node xm_usb3_1 = {
	.name = "xm_usb3_1",
	.id = SDM845_MASTER_USB3_1,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_node qxm_camnoc_hf0_uncomp = {
	.name = "qxm_camnoc_hf0_uncomp",
	.id = SDM845_MASTER_CAMNOC_HF0_UNCOMP,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_CAMNOC_UNCOMP },
};

static struct qcom_icc_node qxm_camnoc_hf1_uncomp = {
	.name = "qxm_camnoc_hf1_uncomp",
	.id = SDM845_MASTER_CAMNOC_HF1_UNCOMP,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_CAMNOC_UNCOMP },
};

static struct qcom_icc_node qxm_camnoc_sf_uncomp = {
	.name = "qxm_camnoc_sf_uncomp",
	.id = SDM845_MASTER_CAMNOC_SF_UNCOMP,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_CAMNOC_UNCOMP },
};

static struct qcom_icc_node qhm_spdm = {
	.name = "qhm_spdm",
	.id = SDM845_MASTER_SPDM,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_CNOC_A2NOC },
};

static struct qcom_icc_node qhm_tic = {
	.name = "qhm_tic",
	.id = SDM845_MASTER_TIC,
	.channels = 1,
	.buswidth = 4,
	.num_links = 43,
	.links = { SDM845_SLAVE_A1NOC_CFG,
		   SDM845_SLAVE_A2NOC_CFG,
		   SDM845_SLAVE_AOP,
		   SDM845_SLAVE_AOSS,
		   SDM845_SLAVE_CAMERA_CFG,
		   SDM845_SLAVE_CLK_CTL,
		   SDM845_SLAVE_CDSP_CFG,
		   SDM845_SLAVE_RBCPR_CX_CFG,
		   SDM845_SLAVE_CRYPTO_0_CFG,
		   SDM845_SLAVE_DCC_CFG,
		   SDM845_SLAVE_CNOC_DDRSS,
		   SDM845_SLAVE_DISPLAY_CFG,
		   SDM845_SLAVE_GLM,
		   SDM845_SLAVE_GFX3D_CFG,
		   SDM845_SLAVE_IMEM_CFG,
		   SDM845_SLAVE_IPA_CFG,
		   SDM845_SLAVE_CNOC_MNOC_CFG,
		   SDM845_SLAVE_PCIE_0_CFG,
		   SDM845_SLAVE_PCIE_1_CFG,
		   SDM845_SLAVE_PDM,
		   SDM845_SLAVE_SOUTH_PHY_CFG,
		   SDM845_SLAVE_PIMEM_CFG,
		   SDM845_SLAVE_PRNG,
		   SDM845_SLAVE_QDSS_CFG,
		   SDM845_SLAVE_BLSP_2,
		   SDM845_SLAVE_BLSP_1,
		   SDM845_SLAVE_SDCC_2,
		   SDM845_SLAVE_SDCC_4,
		   SDM845_SLAVE_SNOC_CFG,
		   SDM845_SLAVE_SPDM_WRAPPER,
		   SDM845_SLAVE_SPSS_CFG,
		   SDM845_SLAVE_TCSR,
		   SDM845_SLAVE_TLMM_NORTH,
		   SDM845_SLAVE_TLMM_SOUTH,
		   SDM845_SLAVE_TSIF,
		   SDM845_SLAVE_UFS_CARD_CFG,
		   SDM845_SLAVE_UFS_MEM_CFG,
		   SDM845_SLAVE_USB3_0,
		   SDM845_SLAVE_USB3_1,
		   SDM845_SLAVE_VENUS_CFG,
		   SDM845_SLAVE_VSENSE_CTRL_CFG,
		   SDM845_SLAVE_CNOC_A2NOC,
		   SDM845_SLAVE_SERVICE_CNOC
	},
};

static struct qcom_icc_node qnm_snoc = {
	.name = "qnm_snoc",
	.id = SDM845_MASTER_SNOC_CNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 42,
	.links = { SDM845_SLAVE_A1NOC_CFG,
		   SDM845_SLAVE_A2NOC_CFG,
		   SDM845_SLAVE_AOP,
		   SDM845_SLAVE_AOSS,
		   SDM845_SLAVE_CAMERA_CFG,
		   SDM845_SLAVE_CLK_CTL,
		   SDM845_SLAVE_CDSP_CFG,
		   SDM845_SLAVE_RBCPR_CX_CFG,
		   SDM845_SLAVE_CRYPTO_0_CFG,
		   SDM845_SLAVE_DCC_CFG,
		   SDM845_SLAVE_CNOC_DDRSS,
		   SDM845_SLAVE_DISPLAY_CFG,
		   SDM845_SLAVE_GLM,
		   SDM845_SLAVE_GFX3D_CFG,
		   SDM845_SLAVE_IMEM_CFG,
		   SDM845_SLAVE_IPA_CFG,
		   SDM845_SLAVE_CNOC_MNOC_CFG,
		   SDM845_SLAVE_PCIE_0_CFG,
		   SDM845_SLAVE_PCIE_1_CFG,
		   SDM845_SLAVE_PDM,
		   SDM845_SLAVE_SOUTH_PHY_CFG,
		   SDM845_SLAVE_PIMEM_CFG,
		   SDM845_SLAVE_PRNG,
		   SDM845_SLAVE_QDSS_CFG,
		   SDM845_SLAVE_BLSP_2,
		   SDM845_SLAVE_BLSP_1,
		   SDM845_SLAVE_SDCC_2,
		   SDM845_SLAVE_SDCC_4,
		   SDM845_SLAVE_SNOC_CFG,
		   SDM845_SLAVE_SPDM_WRAPPER,
		   SDM845_SLAVE_SPSS_CFG,
		   SDM845_SLAVE_TCSR,
		   SDM845_SLAVE_TLMM_NORTH,
		   SDM845_SLAVE_TLMM_SOUTH,
		   SDM845_SLAVE_TSIF,
		   SDM845_SLAVE_UFS_CARD_CFG,
		   SDM845_SLAVE_UFS_MEM_CFG,
		   SDM845_SLAVE_USB3_0,
		   SDM845_SLAVE_USB3_1,
		   SDM845_SLAVE_VENUS_CFG,
		   SDM845_SLAVE_VSENSE_CTRL_CFG,
		   SDM845_SLAVE_SERVICE_CNOC
	},
};

static struct qcom_icc_node xm_qdss_dap = {
	.name = "xm_qdss_dap",
	.id = SDM845_MASTER_QDSS_DAP,
	.channels = 1,
	.buswidth = 8,
	.num_links = 43,
	.links = { SDM845_SLAVE_A1NOC_CFG,
		   SDM845_SLAVE_A2NOC_CFG,
		   SDM845_SLAVE_AOP,
		   SDM845_SLAVE_AOSS,
		   SDM845_SLAVE_CAMERA_CFG,
		   SDM845_SLAVE_CLK_CTL,
		   SDM845_SLAVE_CDSP_CFG,
		   SDM845_SLAVE_RBCPR_CX_CFG,
		   SDM845_SLAVE_CRYPTO_0_CFG,
		   SDM845_SLAVE_DCC_CFG,
		   SDM845_SLAVE_CNOC_DDRSS,
		   SDM845_SLAVE_DISPLAY_CFG,
		   SDM845_SLAVE_GLM,
		   SDM845_SLAVE_GFX3D_CFG,
		   SDM845_SLAVE_IMEM_CFG,
		   SDM845_SLAVE_IPA_CFG,
		   SDM845_SLAVE_CNOC_MNOC_CFG,
		   SDM845_SLAVE_PCIE_0_CFG,
		   SDM845_SLAVE_PCIE_1_CFG,
		   SDM845_SLAVE_PDM,
		   SDM845_SLAVE_SOUTH_PHY_CFG,
		   SDM845_SLAVE_PIMEM_CFG,
		   SDM845_SLAVE_PRNG,
		   SDM845_SLAVE_QDSS_CFG,
		   SDM845_SLAVE_BLSP_2,
		   SDM845_SLAVE_BLSP_1,
		   SDM845_SLAVE_SDCC_2,
		   SDM845_SLAVE_SDCC_4,
		   SDM845_SLAVE_SNOC_CFG,
		   SDM845_SLAVE_SPDM_WRAPPER,
		   SDM845_SLAVE_SPSS_CFG,
		   SDM845_SLAVE_TCSR,
		   SDM845_SLAVE_TLMM_NORTH,
		   SDM845_SLAVE_TLMM_SOUTH,
		   SDM845_SLAVE_TSIF,
		   SDM845_SLAVE_UFS_CARD_CFG,
		   SDM845_SLAVE_UFS_MEM_CFG,
		   SDM845_SLAVE_USB3_0,
		   SDM845_SLAVE_USB3_1,
		   SDM845_SLAVE_VENUS_CFG,
		   SDM845_SLAVE_VSENSE_CTRL_CFG,
		   SDM845_SLAVE_CNOC_A2NOC,
		   SDM845_SLAVE_SERVICE_CNOC
	},
};

static struct qcom_icc_node qhm_cnoc = {
	.name = "qhm_cnoc",
	.id = SDM845_MASTER_CNOC_DC_NOC,
	.channels = 1,
	.buswidth = 4,
	.num_links = 2,
	.links = { SDM845_SLAVE_LLCC_CFG,
		   SDM845_SLAVE_MEM_NOC_CFG
	},
};

static struct qcom_icc_node acm_l3 = {
	.name = "acm_l3",
	.id = SDM845_MASTER_APPSS_PROC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 3,
	.links = { SDM845_SLAVE_GNOC_SNOC,
		   SDM845_SLAVE_GNOC_MEM_NOC,
		   SDM845_SLAVE_SERVICE_GNOC
	},
};

static struct qcom_icc_node pm_gnoc_cfg = {
	.name = "pm_gnoc_cfg",
	.id = SDM845_MASTER_GNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_SERVICE_GNOC },
};

static struct qcom_icc_node llcc_mc = {
	.name = "llcc_mc",
	.id = SDM845_MASTER_LLCC,
	.channels = 4,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_EBI1 },
};

static struct qcom_icc_node acm_tcu = {
	.name = "acm_tcu",
	.id = SDM845_MASTER_TCU_0,
	.channels = 1,
	.buswidth = 8,
	.num_links = 3,
	.links = { SDM845_SLAVE_MEM_NOC_GNOC,
		   SDM845_SLAVE_LLCC,
		   SDM845_SLAVE_MEM_NOC_SNOC
	},
};

static struct qcom_icc_node qhm_memnoc_cfg = {
	.name = "qhm_memnoc_cfg",
	.id = SDM845_MASTER_MEM_NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 2,
	.links = { SDM845_SLAVE_MSS_PROC_MS_MPU_CFG,
		   SDM845_SLAVE_SERVICE_MEM_NOC
	},
};

static struct qcom_icc_node qnm_apps = {
	.name = "qnm_apps",
	.id = SDM845_MASTER_GNOC_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_LLCC },
};

static struct qcom_icc_node qnm_mnoc_hf = {
	.name = "qnm_mnoc_hf",
	.id = SDM845_MASTER_MNOC_HF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.num_links = 2,
	.links = { SDM845_SLAVE_MEM_NOC_GNOC,
		   SDM845_SLAVE_LLCC
	},
};

static struct qcom_icc_node qnm_mnoc_sf = {
	.name = "qnm_mnoc_sf",
	.id = SDM845_MASTER_MNOC_SF_MEM_NOC,
	.channels = 1,
	.buswidth = 32,
	.num_links = 3,
	.links = { SDM845_SLAVE_MEM_NOC_GNOC,
		   SDM845_SLAVE_LLCC,
		   SDM845_SLAVE_MEM_NOC_SNOC
	},
};

static struct qcom_icc_node qnm_snoc_gc = {
	.name = "qnm_snoc_gc",
	.id = SDM845_MASTER_SNOC_GC_MEM_NOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_LLCC },
};

static struct qcom_icc_node qnm_snoc_sf = {
	.name = "qnm_snoc_sf",
	.id = SDM845_MASTER_SNOC_SF_MEM_NOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 2,
	.links = { SDM845_SLAVE_MEM_NOC_GNOC,
		   SDM845_SLAVE_LLCC
	},
};

static struct qcom_icc_node qxm_gpu = {
	.name = "qxm_gpu",
	.id = SDM845_MASTER_GFX3D,
	.channels = 2,
	.buswidth = 32,
	.num_links = 3,
	.links = { SDM845_SLAVE_MEM_NOC_GNOC,
		   SDM845_SLAVE_LLCC,
		   SDM845_SLAVE_MEM_NOC_SNOC
	},
};

static struct qcom_icc_node qhm_mnoc_cfg = {
	.name = "qhm_mnoc_cfg",
	.id = SDM845_MASTER_CNOC_MNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_SERVICE_MNOC },
};

static struct qcom_icc_node qxm_camnoc_hf0 = {
	.name = "qxm_camnoc_hf0",
	.id = SDM845_MASTER_CAMNOC_HF0,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qxm_camnoc_hf1 = {
	.name = "qxm_camnoc_hf1",
	.id = SDM845_MASTER_CAMNOC_HF1,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qxm_camnoc_sf = {
	.name = "qxm_camnoc_sf",
	.id = SDM845_MASTER_CAMNOC_SF,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qxm_mdp0 = {
	.name = "qxm_mdp0",
	.id = SDM845_MASTER_MDP0,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qxm_mdp1 = {
	.name = "qxm_mdp1",
	.id = SDM845_MASTER_MDP1,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qxm_rot = {
	.name = "qxm_rot",
	.id = SDM845_MASTER_ROTATOR,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qxm_venus0 = {
	.name = "qxm_venus0",
	.id = SDM845_MASTER_VIDEO_P0,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qxm_venus1 = {
	.name = "qxm_venus1",
	.id = SDM845_MASTER_VIDEO_P1,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qxm_venus_arm9 = {
	.name = "qxm_venus_arm9",
	.id = SDM845_MASTER_VIDEO_PROC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qhm_snoc_cfg = {
	.name = "qhm_snoc_cfg",
	.id = SDM845_MASTER_SNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_SLAVE_SERVICE_SNOC },
};

static struct qcom_icc_node qnm_aggre1_noc = {
	.name = "qnm_aggre1_noc",
	.id = SDM845_MASTER_A1NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 6,
	.links = { SDM845_SLAVE_APPSS,
		   SDM845_SLAVE_SNOC_CNOC,
		   SDM845_SLAVE_SNOC_MEM_NOC_SF,
		   SDM845_SLAVE_IMEM,
		   SDM845_SLAVE_PIMEM,
		   SDM845_SLAVE_QDSS_STM
	},
};

static struct qcom_icc_node qnm_aggre2_noc = {
	.name = "qnm_aggre2_noc",
	.id = SDM845_MASTER_A2NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 9,
	.links = { SDM845_SLAVE_APPSS,
		   SDM845_SLAVE_SNOC_CNOC,
		   SDM845_SLAVE_SNOC_MEM_NOC_SF,
		   SDM845_SLAVE_IMEM,
		   SDM845_SLAVE_PCIE_0,
		   SDM845_SLAVE_PCIE_1,
		   SDM845_SLAVE_PIMEM,
		   SDM845_SLAVE_QDSS_STM,
		   SDM845_SLAVE_TCU
	},
};

static struct qcom_icc_node qnm_gladiator_sodv = {
	.name = "qnm_gladiator_sodv",
	.id = SDM845_MASTER_GNOC_SNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 8,
	.links = { SDM845_SLAVE_APPSS,
		   SDM845_SLAVE_SNOC_CNOC,
		   SDM845_SLAVE_IMEM,
		   SDM845_SLAVE_PCIE_0,
		   SDM845_SLAVE_PCIE_1,
		   SDM845_SLAVE_PIMEM,
		   SDM845_SLAVE_QDSS_STM,
		   SDM845_SLAVE_TCU
	},
};

static struct qcom_icc_node qnm_memnoc = {
	.name = "qnm_memnoc",
	.id = SDM845_MASTER_MEM_NOC_SNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 5,
	.links = { SDM845_SLAVE_APPSS,
		   SDM845_SLAVE_SNOC_CNOC,
		   SDM845_SLAVE_IMEM,
		   SDM845_SLAVE_PIMEM,
		   SDM845_SLAVE_QDSS_STM
	},
};

static struct qcom_icc_node qnm_pcie_anoc = {
	.name = "qnm_pcie_anoc",
	.id = SDM845_MASTER_ANOC_PCIE_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 5,
	.links = { SDM845_SLAVE_APPSS,
		   SDM845_SLAVE_SNOC_CNOC,
		   SDM845_SLAVE_SNOC_MEM_NOC_SF,
		   SDM845_SLAVE_IMEM,
		   SDM845_SLAVE_QDSS_STM
	},
};

static struct qcom_icc_node qxm_pimem = {
	.name = "qxm_pimem",
	.id = SDM845_MASTER_PIMEM,
	.channels = 1,
	.buswidth = 8,
	.num_links = 2,
	.links = { SDM845_SLAVE_SNOC_MEM_NOC_GC,
		   SDM845_SLAVE_IMEM
	},
};

static struct qcom_icc_node xm_gic = {
	.name = "xm_gic",
	.id = SDM845_MASTER_GIC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 2,
	.links = { SDM845_SLAVE_SNOC_MEM_NOC_GC,
		   SDM845_SLAVE_IMEM
	},
};

static struct qcom_icc_node qns_a1noc_snoc = {
	.name = "qns_a1noc_snoc",
	.id = SDM845_SLAVE_A1NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_A1NOC_SNOC },
};

static struct qcom_icc_node srvc_aggre1_noc = {
	.name = "srvc_aggre1_noc",
	.id = SDM845_SLAVE_SERVICE_A1NOC,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { 0 },
};

static struct qcom_icc_node qns_pcie_a1noc_snoc = {
	.name = "qns_pcie_a1noc_snoc",
	.id = SDM845_SLAVE_ANOC_PCIE_A1NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_ANOC_PCIE_SNOC },
};

static struct qcom_icc_node qns_a2noc_snoc = {
	.name = "qns_a2noc_snoc",
	.id = SDM845_SLAVE_A2NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_A2NOC_SNOC },
};

static struct qcom_icc_node qns_pcie_snoc = {
	.name = "qns_pcie_snoc",
	.id = SDM845_SLAVE_ANOC_PCIE_SNOC,
	.channels = 1,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_ANOC_PCIE_SNOC },
};

static struct qcom_icc_node srvc_aggre2_noc = {
	.name = "srvc_aggre2_noc",
	.id = SDM845_SLAVE_SERVICE_A2NOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qns_camnoc_uncomp = {
	.name = "qns_camnoc_uncomp",
	.id = SDM845_SLAVE_CAMNOC_UNCOMP,
	.channels = 1,
	.buswidth = 32,
};

static struct qcom_icc_node qhs_a1_noc_cfg = {
	.name = "qhs_a1_noc_cfg",
	.id = SDM845_SLAVE_A1NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_A1NOC_CFG },
};

static struct qcom_icc_node qhs_a2_noc_cfg = {
	.name = "qhs_a2_noc_cfg",
	.id = SDM845_SLAVE_A2NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_A2NOC_CFG },
};

static struct qcom_icc_node qhs_aop = {
	.name = "qhs_aop",
	.id = SDM845_SLAVE_AOP,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_aoss = {
	.name = "qhs_aoss",
	.id = SDM845_SLAVE_AOSS,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_camera_cfg = {
	.name = "qhs_camera_cfg",
	.id = SDM845_SLAVE_CAMERA_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_clk_ctl = {
	.name = "qhs_clk_ctl",
	.id = SDM845_SLAVE_CLK_CTL,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_compute_dsp_cfg = {
	.name = "qhs_compute_dsp_cfg",
	.id = SDM845_SLAVE_CDSP_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_cpr_cx = {
	.name = "qhs_cpr_cx",
	.id = SDM845_SLAVE_RBCPR_CX_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_crypto0_cfg = {
	.name = "qhs_crypto0_cfg",
	.id = SDM845_SLAVE_CRYPTO_0_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_dcc_cfg = {
	.name = "qhs_dcc_cfg",
	.id = SDM845_SLAVE_DCC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_CNOC_DC_NOC },
};

static struct qcom_icc_node qhs_ddrss_cfg = {
	.name = "qhs_ddrss_cfg",
	.id = SDM845_SLAVE_CNOC_DDRSS,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_display_cfg = {
	.name = "qhs_display_cfg",
	.id = SDM845_SLAVE_DISPLAY_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_glm = {
	.name = "qhs_glm",
	.id = SDM845_SLAVE_GLM,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_gpuss_cfg = {
	.name = "qhs_gpuss_cfg",
	.id = SDM845_SLAVE_GFX3D_CFG,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node qhs_imem_cfg = {
	.name = "qhs_imem_cfg",
	.id = SDM845_SLAVE_IMEM_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_ipa = {
	.name = "qhs_ipa",
	.id = SDM845_SLAVE_IPA_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_mnoc_cfg = {
	.name = "qhs_mnoc_cfg",
	.id = SDM845_SLAVE_CNOC_MNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_CNOC_MNOC_CFG },
};

static struct qcom_icc_node qhs_pcie0_cfg = {
	.name = "qhs_pcie0_cfg",
	.id = SDM845_SLAVE_PCIE_0_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_pcie_gen3_cfg = {
	.name = "qhs_pcie_gen3_cfg",
	.id = SDM845_SLAVE_PCIE_1_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_pdm = {
	.name = "qhs_pdm",
	.id = SDM845_SLAVE_PDM,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_phy_refgen_south = {
	.name = "qhs_phy_refgen_south",
	.id = SDM845_SLAVE_SOUTH_PHY_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_pimem_cfg = {
	.name = "qhs_pimem_cfg",
	.id = SDM845_SLAVE_PIMEM_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_prng = {
	.name = "qhs_prng",
	.id = SDM845_SLAVE_PRNG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_qdss_cfg = {
	.name = "qhs_qdss_cfg",
	.id = SDM845_SLAVE_QDSS_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_qupv3_north = {
	.name = "qhs_qupv3_north",
	.id = SDM845_SLAVE_BLSP_2,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_qupv3_south = {
	.name = "qhs_qupv3_south",
	.id = SDM845_SLAVE_BLSP_1,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_sdc2 = {
	.name = "qhs_sdc2",
	.id = SDM845_SLAVE_SDCC_2,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_sdc4 = {
	.name = "qhs_sdc4",
	.id = SDM845_SLAVE_SDCC_4,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_snoc_cfg = {
	.name = "qhs_snoc_cfg",
	.id = SDM845_SLAVE_SNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_SNOC_CFG },
};

static struct qcom_icc_node qhs_spdm = {
	.name = "qhs_spdm",
	.id = SDM845_SLAVE_SPDM_WRAPPER,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_spss_cfg = {
	.name = "qhs_spss_cfg",
	.id = SDM845_SLAVE_SPSS_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_tcsr = {
	.name = "qhs_tcsr",
	.id = SDM845_SLAVE_TCSR,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_tlmm_north = {
	.name = "qhs_tlmm_north",
	.id = SDM845_SLAVE_TLMM_NORTH,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_tlmm_south = {
	.name = "qhs_tlmm_south",
	.id = SDM845_SLAVE_TLMM_SOUTH,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_tsif = {
	.name = "qhs_tsif",
	.id = SDM845_SLAVE_TSIF,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_ufs_card_cfg = {
	.name = "qhs_ufs_card_cfg",
	.id = SDM845_SLAVE_UFS_CARD_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_ufs_mem_cfg = {
	.name = "qhs_ufs_mem_cfg",
	.id = SDM845_SLAVE_UFS_MEM_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_usb3_0 = {
	.name = "qhs_usb3_0",
	.id = SDM845_SLAVE_USB3_0,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_usb3_1 = {
	.name = "qhs_usb3_1",
	.id = SDM845_SLAVE_USB3_1,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_venus_cfg = {
	.name = "qhs_venus_cfg",
	.id = SDM845_SLAVE_VENUS_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_vsense_ctrl_cfg = {
	.name = "qhs_vsense_ctrl_cfg",
	.id = SDM845_SLAVE_VSENSE_CTRL_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qns_cnoc_a2noc = {
	.name = "qns_cnoc_a2noc",
	.id = SDM845_SLAVE_CNOC_A2NOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_MASTER_CNOC_A2NOC },
};

static struct qcom_icc_node srvc_cnoc = {
	.name = "srvc_cnoc",
	.id = SDM845_SLAVE_SERVICE_CNOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_llcc = {
	.name = "qhs_llcc",
	.id = SDM845_SLAVE_LLCC_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_memnoc = {
	.name = "qhs_memnoc",
	.id = SDM845_SLAVE_MEM_NOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.num_links = 1,
	.links = { SDM845_MASTER_MEM_NOC_CFG },
};

static struct qcom_icc_node qns_gladiator_sodv = {
	.name = "qns_gladiator_sodv",
	.id = SDM845_SLAVE_GNOC_SNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_MASTER_GNOC_SNOC },
};

static struct qcom_icc_node qns_gnoc_memnoc = {
	.name = "qns_gnoc_memnoc",
	.id = SDM845_SLAVE_GNOC_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_MASTER_GNOC_MEM_NOC },
};

static struct qcom_icc_node srvc_gnoc = {
	.name = "srvc_gnoc",
	.id = SDM845_SLAVE_SERVICE_GNOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node ebi = {
	.name = "ebi",
	.id = SDM845_SLAVE_EBI1,
	.channels = 4,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_mdsp_ms_mpu_cfg = {
	.name = "qhs_mdsp_ms_mpu_cfg",
	.id = SDM845_SLAVE_MSS_PROC_MS_MPU_CFG,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qns_apps_io = {
	.name = "qns_apps_io",
	.id = SDM845_SLAVE_MEM_NOC_GNOC,
	.channels = 1,
	.buswidth = 32,
};

static struct qcom_icc_node qns_llcc = {
	.name = "qns_llcc",
	.id = SDM845_SLAVE_LLCC,
	.channels = 4,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_LLCC },
};

static struct qcom_icc_node qns_memnoc_snoc = {
	.name = "qns_memnoc_snoc",
	.id = SDM845_SLAVE_MEM_NOC_SNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_MASTER_MEM_NOC_SNOC },
};

static struct qcom_icc_node srvc_memnoc = {
	.name = "srvc_memnoc",
	.id = SDM845_SLAVE_SERVICE_MEM_NOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qns2_mem_noc = {
	.name = "qns2_mem_noc",
	.id = SDM845_SLAVE_MNOC_SF_MEM_NOC,
	.channels = 1,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_MASTER_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qns_mem_noc_hf = {
	.name = "qns_mem_noc_hf",
	.id = SDM845_SLAVE_MNOC_HF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.num_links = 1,
	.links = { SDM845_MASTER_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node srvc_mnoc = {
	.name = "srvc_mnoc",
	.id = SDM845_SLAVE_SERVICE_MNOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node qhs_apss = {
	.name = "qhs_apss",
	.id = SDM845_SLAVE_APPSS,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node qns_cnoc = {
	.name = "qns_cnoc",
	.id = SDM845_SLAVE_SNOC_CNOC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_MASTER_SNOC_CNOC },
};

static struct qcom_icc_node qns_memnoc_gc = {
	.name = "qns_memnoc_gc",
	.id = SDM845_SLAVE_SNOC_MEM_NOC_GC,
	.channels = 1,
	.buswidth = 8,
	.num_links = 1,
	.links = { SDM845_MASTER_SNOC_GC_MEM_NOC },
};

static struct qcom_icc_node qns_memnoc_sf = {
	.name = "qns_memnoc_sf",
	.id = SDM845_SLAVE_SNOC_MEM_NOC_SF,
	.channels = 1,
	.buswidth = 16,
	.num_links = 1,
	.links = { SDM845_MASTER_SNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qxs_imem = {
	.name = "qxs_imem",
	.id = SDM845_SLAVE_IMEM,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node qxs_pcie = {
	.name = "qxs_pcie",
	.id = SDM845_SLAVE_PCIE_0,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node qxs_pcie_gen3 = {
	.name = "qxs_pcie_gen3",
	.id = SDM845_SLAVE_PCIE_1,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node qxs_pimem = {
	.name = "qxs_pimem",
	.id = SDM845_SLAVE_PIMEM,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_node srvc_snoc = {
	.name = "srvc_snoc",
	.id = SDM845_SLAVE_SERVICE_SNOC,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node xs_qdss_stm = {
	.name = "xs_qdss_stm",
	.id = SDM845_SLAVE_QDSS_STM,
	.channels = 1,
	.buswidth = 4,
};

static struct qcom_icc_node xs_sys_tcu_cfg = {
	.name = "xs_sys_tcu_cfg",
	.id = SDM845_SLAVE_TCU,
	.channels = 1,
	.buswidth = 8,
};

static struct qcom_icc_bcm bcm_acv = {
	.name = "ACV",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_mc0 = {
	.name = "MC0",
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_sh0 = {
	.name = "SH0",
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &qns_llcc },
};

static struct qcom_icc_bcm bcm_mm0 = {
	.name = "MM0",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns_mem_noc_hf },
};

static struct qcom_icc_bcm bcm_sh1 = {
	.name = "SH1",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns_apps_io },
};

static struct qcom_icc_bcm bcm_mm1 = {
	.name = "MM1",
	.keepalive = true,
	.num_nodes = 7,
	.nodes = { &qxm_camnoc_hf0_uncomp,
		   &qxm_camnoc_hf1_uncomp,
		   &qxm_camnoc_sf_uncomp,
		   &qxm_camnoc_hf0,
		   &qxm_camnoc_hf1,
		   &qxm_mdp0,
		   &qxm_mdp1
	},
};

static struct qcom_icc_bcm bcm_sh2 = {
	.name = "SH2",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns_memnoc_snoc },
};

static struct qcom_icc_bcm bcm_mm2 = {
	.name = "MM2",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns2_mem_noc },
};

static struct qcom_icc_bcm bcm_sh3 = {
	.name = "SH3",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &acm_tcu },
};

static struct qcom_icc_bcm bcm_mm3 = {
	.name = "MM3",
	.keepalive = false,
	.num_nodes = 5,
	.nodes = { &qxm_camnoc_sf, &qxm_rot, &qxm_venus0, &qxm_venus1, &qxm_venus_arm9 },
};

static struct qcom_icc_bcm bcm_sh5 = {
	.name = "SH5",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qnm_apps },
};

static struct qcom_icc_bcm bcm_sn0 = {
	.name = "SN0",
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &qns_memnoc_sf },
};

static struct qcom_icc_bcm bcm_ce0 = {
	.name = "CE0",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qxm_crypto },
};

static struct qcom_icc_bcm bcm_cn0 = {
	.name = "CN0",
	.keepalive = false,
	.num_nodes = 47,
	.nodes = { &qhm_spdm,
		   &qhm_tic,
		   &qnm_snoc,
		   &xm_qdss_dap,
		   &qhs_a1_noc_cfg,
		   &qhs_a2_noc_cfg,
		   &qhs_aop,
		   &qhs_aoss,
		   &qhs_camera_cfg,
		   &qhs_clk_ctl,
		   &qhs_compute_dsp_cfg,
		   &qhs_cpr_cx,
		   &qhs_crypto0_cfg,
		   &qhs_dcc_cfg,
		   &qhs_ddrss_cfg,
		   &qhs_display_cfg,
		   &qhs_glm,
		   &qhs_gpuss_cfg,
		   &qhs_imem_cfg,
		   &qhs_ipa,
		   &qhs_mnoc_cfg,
		   &qhs_pcie0_cfg,
		   &qhs_pcie_gen3_cfg,
		   &qhs_pdm,
		   &qhs_phy_refgen_south,
		   &qhs_pimem_cfg,
		   &qhs_prng,
		   &qhs_qdss_cfg,
		   &qhs_qupv3_north,
		   &qhs_qupv3_south,
		   &qhs_sdc2,
		   &qhs_sdc4,
		   &qhs_snoc_cfg,
		   &qhs_spdm,
		   &qhs_spss_cfg,
		   &qhs_tcsr,
		   &qhs_tlmm_north,
		   &qhs_tlmm_south,
		   &qhs_tsif,
		   &qhs_ufs_card_cfg,
		   &qhs_ufs_mem_cfg,
		   &qhs_usb3_0,
		   &qhs_usb3_1,
		   &qhs_venus_cfg,
		   &qhs_vsense_ctrl_cfg,
		   &qns_cnoc_a2noc,
		   &srvc_cnoc
	},
};

static struct qcom_icc_bcm bcm_qup0 = {
	.name = "QUP0",
	.keepalive = false,
	.num_nodes = 2,
	.nodes = { &qhm_qup1, &qhm_qup2 },
};

static struct qcom_icc_bcm bcm_sn1 = {
	.name = "SN1",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qxs_imem },
};

static struct qcom_icc_bcm bcm_sn2 = {
	.name = "SN2",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns_memnoc_gc },
};

static struct qcom_icc_bcm bcm_sn3 = {
	.name = "SN3",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qns_cnoc },
};

static struct qcom_icc_bcm bcm_sn4 = {
	.name = "SN4",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qxm_pimem },
};

static struct qcom_icc_bcm bcm_sn5 = {
	.name = "SN5",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &xs_qdss_stm },
};

static struct qcom_icc_bcm bcm_sn6 = {
	.name = "SN6",
	.keepalive = false,
	.num_nodes = 3,
	.nodes = { &qhs_apss, &srvc_snoc, &xs_sys_tcu_cfg },
};

static struct qcom_icc_bcm bcm_sn7 = {
	.name = "SN7",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qxs_pcie },
};

static struct qcom_icc_bcm bcm_sn8 = {
	.name = "SN8",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qxs_pcie_gen3 },
};

static struct qcom_icc_bcm bcm_sn9 = {
	.name = "SN9",
	.keepalive = false,
	.num_nodes = 2,
	.nodes = { &srvc_aggre1_noc, &qnm_aggre1_noc },
};

static struct qcom_icc_bcm bcm_sn11 = {
	.name = "SN11",
	.keepalive = false,
	.num_nodes = 2,
	.nodes = { &srvc_aggre2_noc, &qnm_aggre2_noc },
};

static struct qcom_icc_bcm bcm_sn12 = {
	.name = "SN12",
	.keepalive = false,
	.num_nodes = 2,
	.nodes = { &qnm_gladiator_sodv, &xm_gic },
};

static struct qcom_icc_bcm bcm_sn14 = {
	.name = "SN14",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qnm_pcie_anoc },
};

static struct qcom_icc_bcm bcm_sn15 = {
	.name = "SN15",
	.keepalive = false,
	.num_nodes = 1,
	.nodes = { &qnm_memnoc },
};

static struct qcom_icc_bcm * const aggre1_noc_bcms[] = {
	&bcm_sn9,
	&bcm_qup0,
};

static struct qcom_icc_node * const aggre1_noc_nodes[] = {
	[MASTER_A1NOC_CFG] = &qhm_a1noc_cfg,
	[MASTER_TSIF] = &qhm_tsif,
	[MASTER_SDCC_2] = &xm_sdc2,
	[MASTER_SDCC_4] = &xm_sdc4,
	[MASTER_UFS_CARD] = &xm_ufs_card,
	[MASTER_UFS_MEM] = &xm_ufs_mem,
	[MASTER_PCIE_0] = &xm_pcie_0,
	[SLAVE_A1NOC_SNOC] = &qns_a1noc_snoc,
	[SLAVE_SERVICE_A1NOC] = &srvc_aggre1_noc,
	[SLAVE_ANOC_PCIE_A1NOC_SNOC] = &qns_pcie_a1noc_snoc,
	[MASTER_QUP_1] = &qhm_qup1,
};

static const struct qcom_icc_desc sdm845_aggre1_noc = {
	.nodes = aggre1_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre1_noc_nodes),
	.bcms = aggre1_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre1_noc_bcms),
};

static struct qcom_icc_bcm * const aggre2_noc_bcms[] = {
	&bcm_ce0,
	&bcm_sn11,
	&bcm_qup0,
};

static struct qcom_icc_node * const aggre2_noc_nodes[] = {
	[MASTER_A2NOC_CFG] = &qhm_a2noc_cfg,
	[MASTER_QDSS_BAM] = &qhm_qdss_bam,
	[MASTER_CNOC_A2NOC] = &qnm_cnoc,
	[MASTER_CRYPTO] = &qxm_crypto,
	[MASTER_IPA] = &qxm_ipa,
	[MASTER_PCIE_1] = &xm_pcie3_1,
	[MASTER_QDSS_ETR] = &xm_qdss_etr,
	[MASTER_USB3_0] = &xm_usb3_0,
	[MASTER_USB3_1] = &xm_usb3_1,
	[SLAVE_A2NOC_SNOC] = &qns_a2noc_snoc,
	[SLAVE_ANOC_PCIE_SNOC] = &qns_pcie_snoc,
	[SLAVE_SERVICE_A2NOC] = &srvc_aggre2_noc,
	[MASTER_QUP_2] = &qhm_qup2,
};

static const struct qcom_icc_desc sdm845_aggre2_noc = {
	.nodes = aggre2_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre2_noc_nodes),
	.bcms = aggre2_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre2_noc_bcms),
};

static struct qcom_icc_bcm * const config_noc_bcms[] = {
	&bcm_cn0,
};

static struct qcom_icc_node * const config_noc_nodes[] = {
	[MASTER_SPDM] = &qhm_spdm,
	[MASTER_TIC] = &qhm_tic,
	[MASTER_SNOC_CNOC] = &qnm_snoc,
	[MASTER_QDSS_DAP] = &xm_qdss_dap,
	[SLAVE_A1NOC_CFG] = &qhs_a1_noc_cfg,
	[SLAVE_A2NOC_CFG] = &qhs_a2_noc_cfg,
	[SLAVE_AOP] = &qhs_aop,
	[SLAVE_AOSS] = &qhs_aoss,
	[SLAVE_CAMERA_CFG] = &qhs_camera_cfg,
	[SLAVE_CLK_CTL] = &qhs_clk_ctl,
	[SLAVE_CDSP_CFG] = &qhs_compute_dsp_cfg,
	[SLAVE_RBCPR_CX_CFG] = &qhs_cpr_cx,
	[SLAVE_CRYPTO_0_CFG] = &qhs_crypto0_cfg,
	[SLAVE_DCC_CFG] = &qhs_dcc_cfg,
	[SLAVE_CNOC_DDRSS] = &qhs_ddrss_cfg,
	[SLAVE_DISPLAY_CFG] = &qhs_display_cfg,
	[SLAVE_GLM] = &qhs_glm,
	[SLAVE_GFX3D_CFG] = &qhs_gpuss_cfg,
	[SLAVE_IMEM_CFG] = &qhs_imem_cfg,
	[SLAVE_IPA_CFG] = &qhs_ipa,
	[SLAVE_CNOC_MNOC_CFG] = &qhs_mnoc_cfg,
	[SLAVE_PCIE_0_CFG] = &qhs_pcie0_cfg,
	[SLAVE_PCIE_1_CFG] = &qhs_pcie_gen3_cfg,
	[SLAVE_PDM] = &qhs_pdm,
	[SLAVE_SOUTH_PHY_CFG] = &qhs_phy_refgen_south,
	[SLAVE_PIMEM_CFG] = &qhs_pimem_cfg,
	[SLAVE_PRNG] = &qhs_prng,
	[SLAVE_QDSS_CFG] = &qhs_qdss_cfg,
	[SLAVE_BLSP_2] = &qhs_qupv3_north,
	[SLAVE_BLSP_1] = &qhs_qupv3_south,
	[SLAVE_SDCC_2] = &qhs_sdc2,
	[SLAVE_SDCC_4] = &qhs_sdc4,
	[SLAVE_SNOC_CFG] = &qhs_snoc_cfg,
	[SLAVE_SPDM_WRAPPER] = &qhs_spdm,
	[SLAVE_SPSS_CFG] = &qhs_spss_cfg,
	[SLAVE_TCSR] = &qhs_tcsr,
	[SLAVE_TLMM_NORTH] = &qhs_tlmm_north,
	[SLAVE_TLMM_SOUTH] = &qhs_tlmm_south,
	[SLAVE_TSIF] = &qhs_tsif,
	[SLAVE_UFS_CARD_CFG] = &qhs_ufs_card_cfg,
	[SLAVE_UFS_MEM_CFG] = &qhs_ufs_mem_cfg,
	[SLAVE_USB3_0] = &qhs_usb3_0,
	[SLAVE_USB3_1] = &qhs_usb3_1,
	[SLAVE_VENUS_CFG] = &qhs_venus_cfg,
	[SLAVE_VSENSE_CTRL_CFG] = &qhs_vsense_ctrl_cfg,
	[SLAVE_CNOC_A2NOC] = &qns_cnoc_a2noc,
	[SLAVE_SERVICE_CNOC] = &srvc_cnoc,
};

static const struct qcom_icc_desc sdm845_config_noc = {
	.nodes = config_noc_nodes,
	.num_nodes = ARRAY_SIZE(config_noc_nodes),
	.bcms = config_noc_bcms,
	.num_bcms = ARRAY_SIZE(config_noc_bcms),
};

static struct qcom_icc_bcm * const dc_noc_bcms[] = {
};

static struct qcom_icc_node * const dc_noc_nodes[] = {
	[MASTER_CNOC_DC_NOC] = &qhm_cnoc,
	[SLAVE_LLCC_CFG] = &qhs_llcc,
	[SLAVE_MEM_NOC_CFG] = &qhs_memnoc,
};

static const struct qcom_icc_desc sdm845_dc_noc = {
	.nodes = dc_noc_nodes,
	.num_nodes = ARRAY_SIZE(dc_noc_nodes),
	.bcms = dc_noc_bcms,
	.num_bcms = ARRAY_SIZE(dc_noc_bcms),
};

static struct qcom_icc_bcm * const gladiator_noc_bcms[] = {
};

static struct qcom_icc_node * const gladiator_noc_nodes[] = {
	[MASTER_APPSS_PROC] = &acm_l3,
	[MASTER_GNOC_CFG] = &pm_gnoc_cfg,
	[SLAVE_GNOC_SNOC] = &qns_gladiator_sodv,
	[SLAVE_GNOC_MEM_NOC] = &qns_gnoc_memnoc,
	[SLAVE_SERVICE_GNOC] = &srvc_gnoc,
};

static const struct qcom_icc_desc sdm845_gladiator_noc = {
	.nodes = gladiator_noc_nodes,
	.num_nodes = ARRAY_SIZE(gladiator_noc_nodes),
	.bcms = gladiator_noc_bcms,
	.num_bcms = ARRAY_SIZE(gladiator_noc_bcms),
};

static struct qcom_icc_bcm * const mem_noc_bcms[] = {
	&bcm_mc0,
	&bcm_acv,
	&bcm_sh0,
	&bcm_sh1,
	&bcm_sh2,
	&bcm_sh3,
	&bcm_sh5,
};

static struct qcom_icc_node * const mem_noc_nodes[] = {
	[MASTER_TCU_0] = &acm_tcu,
	[MASTER_MEM_NOC_CFG] = &qhm_memnoc_cfg,
	[MASTER_GNOC_MEM_NOC] = &qnm_apps,
	[MASTER_MNOC_HF_MEM_NOC] = &qnm_mnoc_hf,
	[MASTER_MNOC_SF_MEM_NOC] = &qnm_mnoc_sf,
	[MASTER_SNOC_GC_MEM_NOC] = &qnm_snoc_gc,
	[MASTER_SNOC_SF_MEM_NOC] = &qnm_snoc_sf,
	[MASTER_GFX3D] = &qxm_gpu,
	[SLAVE_MSS_PROC_MS_MPU_CFG] = &qhs_mdsp_ms_mpu_cfg,
	[SLAVE_MEM_NOC_GNOC] = &qns_apps_io,
	[SLAVE_LLCC] = &qns_llcc,
	[SLAVE_MEM_NOC_SNOC] = &qns_memnoc_snoc,
	[SLAVE_SERVICE_MEM_NOC] = &srvc_memnoc,
	[MASTER_LLCC] = &llcc_mc,
	[SLAVE_EBI1] = &ebi,
};

static const struct qcom_icc_desc sdm845_mem_noc = {
	.nodes = mem_noc_nodes,
	.num_nodes = ARRAY_SIZE(mem_noc_nodes),
	.bcms = mem_noc_bcms,
	.num_bcms = ARRAY_SIZE(mem_noc_bcms),
};

static struct qcom_icc_bcm * const mmss_noc_bcms[] = {
	&bcm_mm0,
	&bcm_mm1,
	&bcm_mm2,
	&bcm_mm3,
};

static struct qcom_icc_node * const mmss_noc_nodes[] = {
	[MASTER_CNOC_MNOC_CFG] = &qhm_mnoc_cfg,
	[MASTER_CAMNOC_HF0] = &qxm_camnoc_hf0,
	[MASTER_CAMNOC_HF1] = &qxm_camnoc_hf1,
	[MASTER_CAMNOC_SF] = &qxm_camnoc_sf,
	[MASTER_MDP0] = &qxm_mdp0,
	[MASTER_MDP1] = &qxm_mdp1,
	[MASTER_ROTATOR] = &qxm_rot,
	[MASTER_VIDEO_P0] = &qxm_venus0,
	[MASTER_VIDEO_P1] = &qxm_venus1,
	[MASTER_VIDEO_PROC] = &qxm_venus_arm9,
	[SLAVE_MNOC_SF_MEM_NOC] = &qns2_mem_noc,
	[SLAVE_MNOC_HF_MEM_NOC] = &qns_mem_noc_hf,
	[SLAVE_SERVICE_MNOC] = &srvc_mnoc,
	[MASTER_CAMNOC_HF0_UNCOMP] = &qxm_camnoc_hf0_uncomp,
	[MASTER_CAMNOC_HF1_UNCOMP] = &qxm_camnoc_hf1_uncomp,
	[MASTER_CAMNOC_SF_UNCOMP] = &qxm_camnoc_sf_uncomp,
	[SLAVE_CAMNOC_UNCOMP] = &qns_camnoc_uncomp,
};

static const struct qcom_icc_desc sdm845_mmss_noc = {
	.nodes = mmss_noc_nodes,
	.num_nodes = ARRAY_SIZE(mmss_noc_nodes),
	.bcms = mmss_noc_bcms,
	.num_bcms = ARRAY_SIZE(mmss_noc_bcms),
};

static struct qcom_icc_bcm * const system_noc_bcms[] = {
	&bcm_sn0,
	&bcm_sn1,
	&bcm_sn2,
	&bcm_sn3,
	&bcm_sn4,
	&bcm_sn5,
	&bcm_sn6,
	&bcm_sn7,
	&bcm_sn8,
	&bcm_sn9,
	&bcm_sn11,
	&bcm_sn12,
	&bcm_sn14,
	&bcm_sn15,
};

static struct qcom_icc_node * const system_noc_nodes[] = {
	[MASTER_SNOC_CFG] = &qhm_snoc_cfg,
	[MASTER_A1NOC_SNOC] = &qnm_aggre1_noc,
	[MASTER_A2NOC_SNOC] = &qnm_aggre2_noc,
	[MASTER_GNOC_SNOC] = &qnm_gladiator_sodv,
	[MASTER_MEM_NOC_SNOC] = &qnm_memnoc,
	[MASTER_ANOC_PCIE_SNOC] = &qnm_pcie_anoc,
	[MASTER_PIMEM] = &qxm_pimem,
	[MASTER_GIC] = &xm_gic,
	[SLAVE_APPSS] = &qhs_apss,
	[SLAVE_SNOC_CNOC] = &qns_cnoc,
	[SLAVE_SNOC_MEM_NOC_GC] = &qns_memnoc_gc,
	[SLAVE_SNOC_MEM_NOC_SF] = &qns_memnoc_sf,
	[SLAVE_IMEM] = &qxs_imem,
	[SLAVE_PCIE_0] = &qxs_pcie,
	[SLAVE_PCIE_1] = &qxs_pcie_gen3,
	[SLAVE_PIMEM] = &qxs_pimem,
	[SLAVE_SERVICE_SNOC] = &srvc_snoc,
	[SLAVE_QDSS_STM] = &xs_qdss_stm,
	[SLAVE_TCU] = &xs_sys_tcu_cfg,
};

static const struct qcom_icc_desc sdm845_system_noc = {
	.nodes = system_noc_nodes,
	.num_nodes = ARRAY_SIZE(system_noc_nodes),
	.bcms = system_noc_bcms,
	.num_bcms = ARRAY_SIZE(system_noc_bcms),
};

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,sdm845-aggre1-noc",
	  .data = &sdm845_aggre1_noc},
	{ .compatible = "qcom,sdm845-aggre2-noc",
	  .data = &sdm845_aggre2_noc},
	{ .compatible = "qcom,sdm845-config-noc",
	  .data = &sdm845_config_noc},
	{ .compatible = "qcom,sdm845-dc-noc",
	  .data = &sdm845_dc_noc},
	{ .compatible = "qcom,sdm845-gladiator-noc",
	  .data = &sdm845_gladiator_noc},
	{ .compatible = "qcom,sdm845-mem-noc",
	  .data = &sdm845_mem_noc},
	{ .compatible = "qcom,sdm845-mmss-noc",
	  .data = &sdm845_mmss_noc},
	{ .compatible = "qcom,sdm845-system-noc",
	  .data = &sdm845_system_noc},
	{ }
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qcom_icc_rpmh_probe,
	.remove = qcom_icc_rpmh_remove,
	.driver = {
		.name = "qnoc-sdm845",
		.of_match_table = qnoc_of_match,
		.sync_state = icc_sync_state,
	},
};
module_platform_driver(qnoc_driver);

MODULE_AUTHOR("David Dai <daidavid1@codeaurora.org>");
MODULE_DESCRIPTION("Qualcomm sdm845 NoC driver");
MODULE_LICENSE("GPL v2");
