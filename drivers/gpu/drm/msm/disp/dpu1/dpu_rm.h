/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#ifndef __DPU_RM_H__
#define __DPU_RM_H__

#include <linux/list.h>

#include "msm_kms.h"
#include "dpu_hw_top.h"

struct dpu_global_state;

/**
 * struct dpu_rm - DPU dynamic hardware resource manager
 * @pingpong_blks: array of pingpong hardware resources
 * @mixer_blks: array of layer mixer hardware resources
 * @ctl_blks: array of ctl hardware resources
 * @hw_intf: array of intf hardware resources
 * @hw_wb: array of wb hardware resources
 * @dspp_blks: array of dspp hardware resources
 * @hw_sspp: array of sspp hardware resources
 */
struct dpu_rm {
	struct dpu_hw_blk *pingpong_blks[PINGPONG_MAX - PINGPONG_0];
	struct dpu_hw_blk *mixer_blks[LM_MAX - LM_0];
	struct dpu_hw_blk *ctl_blks[CTL_MAX - CTL_0];
	struct dpu_hw_intf *hw_intf[INTF_MAX - INTF_0];
	struct dpu_hw_wb *hw_wb[WB_MAX - WB_0];
	struct dpu_hw_blk *dspp_blks[DSPP_MAX - DSPP_0];
	struct dpu_hw_blk *merge_3d_blks[MERGE_3D_MAX - MERGE_3D_0];
	struct dpu_hw_blk *dsc_blks[DSC_MAX - DSC_0];
	struct dpu_hw_sspp *hw_sspp[SSPP_MAX - SSPP_NONE];
};

/**
 * dpu_rm_init - Read hardware catalog and create reservation tracking objects
 *	for all HW blocks.
 * @rm: DPU Resource Manager handle
 * @cat: Pointer to hardware catalog
 * @mdss_data: Pointer to MDSS / UBWC configuration
 * @mmio: mapped register io address of MDP
 * @Return: 0 on Success otherwise -ERROR
 */
int dpu_rm_init(struct dpu_rm *rm,
		const struct dpu_mdss_cfg *cat,
		const struct msm_mdss_data *mdss_data,
		void __iomem *mmio);

/**
 * dpu_rm_destroy - Free all memory allocated by dpu_rm_init
 * @rm: DPU Resource Manager handle
 * @Return: 0 on Success otherwise -ERROR
 */
int dpu_rm_destroy(struct dpu_rm *rm);

/**
 * dpu_rm_reserve - Given a CRTC->Encoder->Connector display chain, analyze
 *	the use connections and user requirements, specified through related
 *	topology control properties, and reserve hardware blocks to that
 *	display chain.
 *	HW blocks can then be accessed through dpu_rm_get_* functions.
 *	HW Reservations should be released via dpu_rm_release_hw.
 * @rm: DPU Resource Manager handle
 * @drm_enc: DRM Encoder handle
 * @crtc_state: Proposed Atomic DRM CRTC State handle
 * @topology: Pointer to topology info for the display
 * @Return: 0 on Success otherwise -ERROR
 */
int dpu_rm_reserve(struct dpu_rm *rm,
		struct dpu_global_state *global_state,
		struct drm_encoder *drm_enc,
		struct drm_crtc_state *crtc_state,
		struct msm_display_topology topology);

/**
 * dpu_rm_reserve - Given the encoder for the display chain, release any
 *	HW blocks previously reserved for that use case.
 * @rm: DPU Resource Manager handle
 * @enc: DRM Encoder handle
 * @Return: 0 on Success otherwise -ERROR
 */
void dpu_rm_release(struct dpu_global_state *global_state,
		struct drm_encoder *enc);

/**
 * Get hw resources of the given type that are assigned to this encoder.
 */
int dpu_rm_get_assigned_resources(struct dpu_rm *rm,
	struct dpu_global_state *global_state, uint32_t enc_id,
	enum dpu_hw_blk_type type, struct dpu_hw_blk **blks, int blks_size);

/**
 * dpu_rm_get_intf - Return a struct dpu_hw_intf instance given it's index.
 * @rm: DPU Resource Manager handle
 * @intf_idx: INTF's index
 */
static inline struct dpu_hw_intf *dpu_rm_get_intf(struct dpu_rm *rm, enum dpu_intf intf_idx)
{
	return rm->hw_intf[intf_idx - INTF_0];
}

/**
 * dpu_rm_get_wb - Return a struct dpu_hw_wb instance given it's index.
 * @rm: DPU Resource Manager handle
 * @wb_idx: WB index
 */
static inline struct dpu_hw_wb *dpu_rm_get_wb(struct dpu_rm *rm, enum dpu_wb wb_idx)
{
	return rm->hw_wb[wb_idx - WB_0];
}

/**
 * dpu_rm_get_sspp - Return a struct dpu_hw_sspp instance given it's index.
 * @rm: DPU Resource Manager handle
 * @sspp_idx: SSPP index
 */
static inline struct dpu_hw_sspp *dpu_rm_get_sspp(struct dpu_rm *rm, enum dpu_sspp sspp_idx)
{
	return rm->hw_sspp[sspp_idx - SSPP_NONE];
}

#endif /* __DPU_RM_H__ */

