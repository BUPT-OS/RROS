/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef __MTK_MMSYS_H
#define __MTK_MMSYS_H

#include <linux/mailbox_controller.h>
#include <linux/mailbox/mtk-cmdq-mailbox.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

enum mtk_ddp_comp_id;
struct device;

enum mtk_dpi_out_format_con {
	MTK_DPI_RGB888_SDR_CON,
	MTK_DPI_RGB888_DDR_CON,
	MTK_DPI_RGB565_SDR_CON,
	MTK_DPI_RGB565_DDR_CON
};

enum mtk_ddp_comp_id {
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_AAL1,
	DDP_COMPONENT_BLS,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_COLOR1,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DITHER1,
	DDP_COMPONENT_DP_INTF0,
	DDP_COMPONENT_DP_INTF1,
	DDP_COMPONENT_DPI0,
	DDP_COMPONENT_DPI1,
	DDP_COMPONENT_DSC0,
	DDP_COMPONENT_DSC1,
	DDP_COMPONENT_DSI0,
	DDP_COMPONENT_DSI1,
	DDP_COMPONENT_DSI2,
	DDP_COMPONENT_DSI3,
	DDP_COMPONENT_ETHDR_MIXER,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_MDP_RDMA0,
	DDP_COMPONENT_MDP_RDMA1,
	DDP_COMPONENT_MDP_RDMA2,
	DDP_COMPONENT_MDP_RDMA3,
	DDP_COMPONENT_MDP_RDMA4,
	DDP_COMPONENT_MDP_RDMA5,
	DDP_COMPONENT_MDP_RDMA6,
	DDP_COMPONENT_MDP_RDMA7,
	DDP_COMPONENT_MERGE0,
	DDP_COMPONENT_MERGE1,
	DDP_COMPONENT_MERGE2,
	DDP_COMPONENT_MERGE3,
	DDP_COMPONENT_MERGE4,
	DDP_COMPONENT_MERGE5,
	DDP_COMPONENT_OD0,
	DDP_COMPONENT_OD1,
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL_2L0,
	DDP_COMPONENT_OVL_2L1,
	DDP_COMPONENT_OVL_2L2,
	DDP_COMPONENT_OVL1,
	DDP_COMPONENT_POSTMASK0,
	DDP_COMPONENT_PWM0,
	DDP_COMPONENT_PWM1,
	DDP_COMPONENT_PWM2,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_RDMA2,
	DDP_COMPONENT_RDMA4,
	DDP_COMPONENT_UFOE,
	DDP_COMPONENT_WDMA0,
	DDP_COMPONENT_WDMA1,
	DDP_COMPONENT_ID_MAX,
};

void mtk_mmsys_ddp_connect(struct device *dev,
			   enum mtk_ddp_comp_id cur,
			   enum mtk_ddp_comp_id next);

void mtk_mmsys_ddp_disconnect(struct device *dev,
			      enum mtk_ddp_comp_id cur,
			      enum mtk_ddp_comp_id next);

void mtk_mmsys_ddp_dpi_fmt_config(struct device *dev, u32 val);

void mtk_mmsys_merge_async_config(struct device *dev, int idx, int width,
				  int height, struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_hdr_config(struct device *dev, int be_width, int be_height,
			  struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_mixer_in_config(struct device *dev, int idx, bool alpha_sel, u16 alpha,
			       u8 mode, u32 biwidth, struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_mixer_in_channel_swap(struct device *dev, int idx, bool channel_swap,
				     struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_vpp_rsz_merge_config(struct device *dev, u32 id, bool enable,
				    struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_vpp_rsz_dcm_config(struct device *dev, bool enable,
				  struct cmdq_pkt *cmdq_pkt);

#endif /* __MTK_MMSYS_H */
