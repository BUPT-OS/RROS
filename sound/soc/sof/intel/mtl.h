/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2020-2022 Intel Corporation. All rights reserved.
 */

/* HDA Registers */
#define MTL_PPLCLLPL_BASE		0x948
#define MTL_PPLCLLPU_STRIDE		0x10
#define MTL_PPLCLLPL(x)			(MTL_PPLCLLPL_BASE + (x) * MTL_PPLCLLPU_STRIDE)
#define MTL_PPLCLLPU(x)			(MTL_PPLCLLPL_BASE + 0x4 + (x) * MTL_PPLCLLPU_STRIDE)

/* DSP Registers */
#define MTL_HFDSSCS			0x1000
#define MTL_HFDSSCS_SPA_MASK		BIT(16)
#define MTL_HFDSSCS_CPA_MASK		BIT(24)
#define MTL_HFSNDWIE			0x114C
#define MTL_HFPWRCTL			0x1D18
#define MTL_HfPWRCTL_WPIOXPG(x)		BIT((x) + 8)
#define MTL_HFPWRCTL_WPDSPHPXPG		BIT(0)
#define MTL_HFPWRSTS			0x1D1C
#define MTL_HFPWRSTS_DSPHPXPGS_MASK	BIT(0)
#define MTL_HFINTIPPTR			0x1108
#define MTL_IRQ_INTEN_L_HOST_IPC_MASK	BIT(0)
#define MTL_IRQ_INTEN_L_SOUNDWIRE_MASK	BIT(6)
#define MTL_HFINTIPPTR_PTR_MASK		GENMASK(20, 0)

#define MTL_HDA_VS_D0I3C		0x1D4A

#define MTL_DSP2CXCAP_PRIMARY_CORE	0x178D00
#define MTL_DSP2CXCTL_PRIMARY_CORE	0x178D04
#define MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK BIT(0)
#define MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK BIT(8)
#define MTL_DSP2CXCTL_PRIMARY_CORE_OSEL GENMASK(25, 24)
#define MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT 24

/* IPC Registers */
#define MTL_DSP_REG_HFIPCXTDR		0x73200
#define MTL_DSP_REG_HFIPCXTDR_BUSY	BIT(31)
#define MTL_DSP_REG_HFIPCXTDR_MSG_MASK GENMASK(30, 0)
#define MTL_DSP_REG_HFIPCXTDA		0x73204
#define MTL_DSP_REG_HFIPCXTDA_BUSY	BIT(31)
#define MTL_DSP_REG_HFIPCXIDR		0x73210
#define MTL_DSP_REG_HFIPCXIDR_BUSY	BIT(31)
#define MTL_DSP_REG_HFIPCXIDR_MSG_MASK GENMASK(30, 0)
#define MTL_DSP_REG_HFIPCXIDA		0x73214
#define MTL_DSP_REG_HFIPCXIDA_DONE	BIT(31)
#define MTL_DSP_REG_HFIPCXIDA_MSG_MASK GENMASK(30, 0)
#define MTL_DSP_REG_HFIPCXCTL		0x73228
#define MTL_DSP_REG_HFIPCXCTL_BUSY	BIT(0)
#define MTL_DSP_REG_HFIPCXCTL_DONE	BIT(1)
#define MTL_DSP_REG_HFIPCXTDDY		0x73300
#define MTL_DSP_REG_HFIPCXIDDY		0x73380
#define MTL_DSP_REG_HfHIPCIE		0x1140
#define MTL_DSP_REG_HfHIPCIE_IE_MASK	BIT(0)
#define MTL_DSP_REG_HfSNDWIE		0x114C
#define MTL_DSP_REG_HfSNDWIE_IE_MASK	GENMASK(3, 0)

#define MTL_DSP_IRQSTS			0x20
#define MTL_DSP_IRQSTS_IPC		BIT(0)
#define MTL_DSP_IRQSTS_SDW		BIT(6)

#define MTL_DSP_REG_POLL_INTERVAL_US	10	/* 10 us */

/* Memory windows */
#define MTL_SRAM_WINDOW_OFFSET(x)	(0x180000 + 0x8000 * (x))

#define MTL_DSP_MBOX_UPLINK_OFFSET	(MTL_SRAM_WINDOW_OFFSET(0) + 0x1000)
#define MTL_DSP_MBOX_UPLINK_SIZE	0x1000
#define MTL_DSP_MBOX_DOWNLINK_OFFSET	MTL_SRAM_WINDOW_OFFSET(1)
#define MTL_DSP_MBOX_DOWNLINK_SIZE	0x1000

/* FW registers */
#define MTL_DSP_ROM_STS			MTL_SRAM_WINDOW_OFFSET(0) /* ROM status */
#define MTL_DSP_ROM_ERROR		(MTL_SRAM_WINDOW_OFFSET(0) + 0x4) /* ROM error code */

#define MTL_DSP_REG_HFFLGPXQWY		0x163200 /* ROM debug status */
#define MTL_DSP_REG_HFFLGPXQWY_ERROR	0x163204 /* ROM debug error code */
#define MTL_DSP_REG_HfIMRIS1		0x162088
#define MTL_DSP_REG_HfIMRIS1_IU_MASK	BIT(0)

bool mtl_dsp_check_ipc_irq(struct snd_sof_dev *sdev);
int mtl_ipc_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg);

void mtl_enable_ipc_interrupts(struct snd_sof_dev *sdev);
void mtl_disable_ipc_interrupts(struct snd_sof_dev *sdev);

int mtl_enable_interrupts(struct snd_sof_dev *sdev, bool enable);

int mtl_dsp_pre_fw_run(struct snd_sof_dev *sdev);
int mtl_dsp_post_fw_run(struct snd_sof_dev *sdev);
void mtl_dsp_dump(struct snd_sof_dev *sdev, u32 flags);

int mtl_power_down_dsp(struct snd_sof_dev *sdev);
int mtl_dsp_cl_init(struct snd_sof_dev *sdev, int stream_tag, bool imr_boot);

irqreturn_t mtl_ipc_irq_thread(int irq, void *context);

int mtl_dsp_ipc_get_mailbox_offset(struct snd_sof_dev *sdev);
int mtl_dsp_ipc_get_window_offset(struct snd_sof_dev *sdev, u32 id);

void mtl_ipc_dump(struct snd_sof_dev *sdev);

u64 mtl_dsp_get_stream_hda_link_position(struct snd_sof_dev *sdev,
					 struct snd_soc_component *component,
					 struct snd_pcm_substream *substream);
