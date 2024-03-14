/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2018, 2020 The Linux Foundation. All rights reserved.
 */

#ifndef _DPU_HW_CATALOG_H
#define _DPU_HW_CATALOG_H

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/bitmap.h>
#include <linux/err.h>

/**
 * Max hardware block count: For ex: max 12 SSPP pipes or
 * 5 ctl paths. In all cases, it can have max 12 hardware blocks
 * based on current design
 */
#define MAX_BLOCKS    12

#define DPU_HW_BLK_NAME_LEN	16

#define MAX_IMG_WIDTH 0x3fff
#define MAX_IMG_HEIGHT 0x3fff

#define CRTC_DUAL_MIXERS	2

#define MAX_XIN_COUNT 16

/**
 * MDP TOP BLOCK features
 * @DPU_MDP_PANIC_PER_PIPE Panic configuration needs to be done per pipe
 * @DPU_MDP_10BIT_SUPPORT, Chipset supports 10 bit pixel formats
 * @DPU_MDP_PERIPH_0_REMOVED Indicates that access to periph top0 block results
 *			   in a failure
 * @DPU_MDP_VSYNC_SEL      Enables vsync source selection via MDP_VSYNC_SEL register
 *                         (moved into INTF block since DPU 5.0.0)
 * @DPU_MDP_MAX            Maximum value

 */
enum {
	DPU_MDP_PANIC_PER_PIPE = 0x1,
	DPU_MDP_10BIT_SUPPORT,
	DPU_MDP_AUDIO_SELECT,
	DPU_MDP_PERIPH_0_REMOVED,
	DPU_MDP_VSYNC_SEL,
	DPU_MDP_MAX
};

/**
 * SSPP sub-blocks/features
 * @DPU_SSPP_SCALER_QSEED2,  QSEED2 algorithm support
 * @DPU_SSPP_SCALER_QSEED3,  QSEED3 alogorithm support
 * @DPU_SSPP_SCALER_QSEED3LITE,  QSEED3 Lite alogorithm support
 * @DPU_SSPP_SCALER_QSEED4,  QSEED4 algorithm support
 * @DPU_SSPP_SCALER_RGB,     RGB Scaler, supported by RGB pipes
 * @DPU_SSPP_CSC,            Support of Color space converion
 * @DPU_SSPP_CSC_10BIT,      Support of 10-bit Color space conversion
 * @DPU_SSPP_CURSOR,         SSPP can be used as a cursor layer
 * @DPU_SSPP_QOS,            SSPP support QoS control, danger/safe/creq
 * @DPU_SSPP_QOS_8LVL,       SSPP support 8-level QoS control
 * @DPU_SSPP_EXCL_RECT,      SSPP supports exclusion rect
 * @DPU_SSPP_SMART_DMA_V1,   SmartDMA 1.0 support
 * @DPU_SSPP_SMART_DMA_V2,   SmartDMA 2.0 support
 * @DPU_SSPP_TS_PREFILL      Supports prefill with traffic shaper
 * @DPU_SSPP_TS_PREFILL_REC1 Supports prefill with traffic shaper multirec
 * @DPU_SSPP_CDP             Supports client driven prefetch
 * @DPU_SSPP_INLINE_ROTATION Support inline rotation
 * @DPU_SSPP_MAX             maximum value
 */
enum {
	DPU_SSPP_SCALER_QSEED2 = 0x1,
	DPU_SSPP_SCALER_QSEED3,
	DPU_SSPP_SCALER_QSEED3LITE,
	DPU_SSPP_SCALER_QSEED4,
	DPU_SSPP_SCALER_RGB,
	DPU_SSPP_CSC,
	DPU_SSPP_CSC_10BIT,
	DPU_SSPP_CURSOR,
	DPU_SSPP_QOS,
	DPU_SSPP_QOS_8LVL,
	DPU_SSPP_EXCL_RECT,
	DPU_SSPP_SMART_DMA_V1,
	DPU_SSPP_SMART_DMA_V2,
	DPU_SSPP_TS_PREFILL,
	DPU_SSPP_TS_PREFILL_REC1,
	DPU_SSPP_CDP,
	DPU_SSPP_INLINE_ROTATION,
	DPU_SSPP_MAX
};

/*
 * MIXER sub-blocks/features
 * @DPU_MIXER_LAYER           Layer mixer layer blend configuration,
 * @DPU_MIXER_SOURCESPLIT     Layer mixer supports source-split configuration
 * @DPU_MIXER_GC              Gamma correction block
 * @DPU_DIM_LAYER             Layer mixer supports dim layer
 * @DPU_MIXER_COMBINED_ALPHA  Layer mixer has combined alpha register
 * @DPU_MIXER_MAX             maximum value
 */
enum {
	DPU_MIXER_LAYER = 0x1,
	DPU_MIXER_SOURCESPLIT,
	DPU_MIXER_GC,
	DPU_DIM_LAYER,
	DPU_MIXER_COMBINED_ALPHA,
	DPU_MIXER_MAX
};

/**
 * DSPP sub-blocks
 * @DPU_DSPP_PCC             Panel color correction block
 */
enum {
	DPU_DSPP_PCC = 0x1,
	DPU_DSPP_MAX
};

/**
 * PINGPONG sub-blocks
 * @DPU_PINGPONG_TE         Tear check block
 * @DPU_PINGPONG_TE2        Additional tear check block for split pipes
 * @DPU_PINGPONG_SPLIT      PP block supports split fifo
 * @DPU_PINGPONG_SLAVE      PP block is a suitable slave for split fifo
 * @DPU_PINGPONG_DITHER     Dither blocks
 * @DPU_PINGPONG_DSC        PP block supports DSC
 * @DPU_PINGPONG_MAX
 */
enum {
	DPU_PINGPONG_TE = 0x1,
	DPU_PINGPONG_TE2,
	DPU_PINGPONG_SPLIT,
	DPU_PINGPONG_SLAVE,
	DPU_PINGPONG_DITHER,
	DPU_PINGPONG_DSC,
	DPU_PINGPONG_MAX
};

/**
 * CTL sub-blocks
 * @DPU_CTL_SPLIT_DISPLAY:	CTL supports video mode split display
 * @DPU_CTL_FETCH_ACTIVE:	Active CTL for fetch HW (SSPPs)
 * @DPU_CTL_VM_CFG:		CTL config to support multiple VMs
 * @DPU_CTL_HAS_LAYER_EXT4:	CTL has the CTL_LAYER_EXT4 register
 * @DPU_CTL_DSPP_BLOCK_FLUSH:	CTL config to support dspp sub-block flush
 * @DPU_CTL_MAX
 */
enum {
	DPU_CTL_SPLIT_DISPLAY = 0x1,
	DPU_CTL_ACTIVE_CFG,
	DPU_CTL_FETCH_ACTIVE,
	DPU_CTL_VM_CFG,
	DPU_CTL_HAS_LAYER_EXT4,
	DPU_CTL_DSPP_SUB_BLOCK_FLUSH,
	DPU_CTL_MAX
};

/**
 * INTF sub-blocks
 * @DPU_INTF_INPUT_CTRL             Supports the setting of pp block from which
 *                                  pixel data arrives to this INTF
 * @DPU_INTF_TE                     INTF block has TE configuration support
 * @DPU_DATA_HCTL_EN                Allows data to be transferred at different rate
 *                                  than video timing
 * @DPU_INTF_STATUS_SUPPORTED       INTF block has INTF_STATUS register
 * @DPU_INTF_MAX
 */
enum {
	DPU_INTF_INPUT_CTRL = 0x1,
	DPU_INTF_TE,
	DPU_DATA_HCTL_EN,
	DPU_INTF_STATUS_SUPPORTED,
	DPU_INTF_MAX
};

/**
  * WB sub-blocks and features
  * @DPU_WB_LINE_MODE        Writeback module supports line/linear mode
  * @DPU_WB_BLOCK_MODE       Writeback module supports block mode read
  * @DPU_WB_CHROMA_DOWN,     Writeback chroma down block,
  * @DPU_WB_DOWNSCALE,       Writeback integer downscaler,
  * @DPU_WB_DITHER,          Dither block
  * @DPU_WB_TRAFFIC_SHAPER,  Writeback traffic shaper bloc
  * @DPU_WB_UBWC,            Writeback Universal bandwidth compression
  * @DPU_WB_YUV_CONFIG       Writeback supports output of YUV colorspace
  * @DPU_WB_PIPE_ALPHA       Writeback supports pipe alpha
  * @DPU_WB_XY_ROI_OFFSET    Writeback supports x/y-offset of out ROI in
  *                          the destination image
  * @DPU_WB_QOS,             Writeback supports QoS control, danger/safe/creq
  * @DPU_WB_QOS_8LVL,        Writeback supports 8-level QoS control
  * @DPU_WB_CDP              Writeback supports client driven prefetch
  * @DPU_WB_INPUT_CTRL       Writeback supports from which pp block input pixel
  *                          data arrives.
  * @DPU_WB_CROP             CWB supports cropping
  * @DPU_WB_MAX              maximum value
  */
enum {
	DPU_WB_LINE_MODE = 0x1,
	DPU_WB_BLOCK_MODE,
	DPU_WB_UBWC,
	DPU_WB_YUV_CONFIG,
	DPU_WB_PIPE_ALPHA,
	DPU_WB_XY_ROI_OFFSET,
	DPU_WB_QOS,
	DPU_WB_QOS_8LVL,
	DPU_WB_CDP,
	DPU_WB_INPUT_CTRL,
	DPU_WB_CROP,
	DPU_WB_MAX
};

/**
 * VBIF sub-blocks and features
 * @DPU_VBIF_QOS_OTLIM        VBIF supports OT Limit
 * @DPU_VBIF_QOS_REMAP        VBIF supports QoS priority remap
 * @DPU_VBIF_MAX              maximum value
 */
enum {
	DPU_VBIF_QOS_OTLIM = 0x1,
	DPU_VBIF_QOS_REMAP,
	DPU_VBIF_MAX
};

/**
 * DSC sub-blocks/features
 * @DPU_DSC_OUTPUT_CTRL       Configure which PINGPONG block gets
 *                            the pixel output from this DSC.
 * @DPU_DSC_HW_REV_1_2        DSC block supports DSC 1.1 and 1.2
 * @DPU_DSC_NATIVE_42x_EN     Supports NATIVE_422_EN and NATIVE_420_EN encoding
 * @DPU_DSC_MAX
 */
enum {
	DPU_DSC_OUTPUT_CTRL = 0x1,
	DPU_DSC_HW_REV_1_2,
	DPU_DSC_NATIVE_42x_EN,
	DPU_DSC_MAX
};

/**
 * MACRO DPU_HW_BLK_INFO - information of HW blocks inside DPU
 * @name:              string name for debug purposes
 * @id:                enum identifying this block
 * @base:              register base offset to mdss
 * @len:               length of hardware block
 * @features           bit mask identifying sub-blocks/features
 */
#define DPU_HW_BLK_INFO \
	char name[DPU_HW_BLK_NAME_LEN]; \
	u32 id; \
	u32 base; \
	u32 len; \
	unsigned long features

/**
 * MACRO DPU_HW_SUBBLK_INFO - information of HW sub-block inside DPU
 * @name:              string name for debug purposes
 * @id:                enum identifying this sub-block
 * @base:              offset of this sub-block relative to the block
 *                     offset
 * @len                register block length of this sub-block
 */
#define DPU_HW_SUBBLK_INFO \
	char name[DPU_HW_BLK_NAME_LEN]; \
	u32 id; \
	u32 base; \
	u32 len

/**
 * struct dpu_scaler_blk: Scaler information
 * @info:   HW register and features supported by this sub-blk
 * @version: qseed block revision
 */
struct dpu_scaler_blk {
	DPU_HW_SUBBLK_INFO;
	u32 version;
};

struct dpu_csc_blk {
	DPU_HW_SUBBLK_INFO;
};

/**
 * struct dpu_pp_blk : Pixel processing sub-blk information
 * @info:   HW register and features supported by this sub-blk
 * @version: HW Algorithm version
 */
struct dpu_pp_blk {
	DPU_HW_SUBBLK_INFO;
	u32 version;
};

/**
 * struct dpu_dsc_blk - DSC Encoder sub-blk information
 * @info:   HW register and features supported by this sub-blk
 */
struct dpu_dsc_blk {
	DPU_HW_SUBBLK_INFO;
};

/**
 * enum dpu_qos_lut_usage - define QoS LUT use cases
 */
enum dpu_qos_lut_usage {
	DPU_QOS_LUT_USAGE_LINEAR,
	DPU_QOS_LUT_USAGE_MACROTILE,
	DPU_QOS_LUT_USAGE_NRT,
	DPU_QOS_LUT_USAGE_MAX,
};

/**
 * struct dpu_qos_lut_entry - define QoS LUT table entry
 * @fl: fill level, or zero on last entry to indicate default lut
 * @lut: lut to use if equal to or less than fill level
 */
struct dpu_qos_lut_entry {
	u32 fl;
	u64 lut;
};

/**
 * struct dpu_qos_lut_tbl - define QoS LUT table
 * @nentry: number of entry in this table
 * @entries: Pointer to table entries
 */
struct dpu_qos_lut_tbl {
	u32 nentry;
	const struct dpu_qos_lut_entry *entries;
};

/**
 * struct dpu_rotation_cfg - define inline rotation config
 * @rot_maxheight: max pre rotated height allowed for rotation
 * @rot_num_formats: number of elements in @rot_format_list
 * @rot_format_list: list of supported rotator formats
 */
struct dpu_rotation_cfg {
	u32 rot_maxheight;
	size_t rot_num_formats;
	const u32 *rot_format_list;
};

/**
 * struct dpu_caps - define DPU capabilities
 * @max_mixer_width    max layer mixer line width support.
 * @max_mixer_blendstages max layer mixer blend stages or
 *                       supported z order
 * @qseed_type         qseed2 or qseed3 support.
 * @has_src_split      source split feature status
 * @has_dim_layer      dim layer feature status
 * @has_idle_pc        indicate if idle power collapse feature is supported
 * @has_3d_merge       indicate if 3D merge is supported
 * @max_linewidth      max linewidth for sspp
 * @pixel_ram_size     size of latency hiding and de-tiling buffer in bytes
 * @max_hdeci_exp      max horizontal decimation supported (max is 2^value)
 * @max_vdeci_exp      max vertical decimation supported (max is 2^value)
 */
struct dpu_caps {
	u32 max_mixer_width;
	u32 max_mixer_blendstages;
	u32 qseed_type;
	bool has_src_split;
	bool has_dim_layer;
	bool has_idle_pc;
	bool has_3d_merge;
	/* SSPP limits */
	u32 max_linewidth;
	u32 pixel_ram_size;
	u32 max_hdeci_exp;
	u32 max_vdeci_exp;
};

/**
 * struct dpu_sspp_sub_blks : SSPP sub-blocks
 * common: Pointer to common configurations shared by sub blocks
 * @maxdwnscale: max downscale ratio supported(without DECIMATION)
 * @maxupscale:  maxupscale ratio supported
 * @smart_dma_priority: hw priority of rect1 of multirect pipe
 * @max_per_pipe_bw: maximum allowable bandwidth of this pipe in kBps
 * @qseed_ver: qseed version
 * @scaler_blk:
 * @csc_blk:
 * @format_list: Pointer to list of supported formats
 * @num_formats: Number of supported formats
 * @virt_format_list: Pointer to list of supported formats for virtual planes
 * @virt_num_formats: Number of supported formats for virtual planes
 * @dpu_rotation_cfg: inline rotation configuration
 */
struct dpu_sspp_sub_blks {
	u32 maxdwnscale;
	u32 maxupscale;
	u32 smart_dma_priority;
	u32 max_per_pipe_bw;
	u32 qseed_ver;
	struct dpu_scaler_blk scaler_blk;
	struct dpu_pp_blk csc_blk;

	const u32 *format_list;
	u32 num_formats;
	const u32 *virt_format_list;
	u32 virt_num_formats;
	const struct dpu_rotation_cfg *rotation_cfg;
};

/**
 * struct dpu_lm_sub_blks:      information of mixer block
 * @maxwidth:               Max pixel width supported by this mixer
 * @maxblendstages:         Max number of blend-stages supported
 * @blendstage_base:        Blend-stage register base offset
 */
struct dpu_lm_sub_blks {
	u32 maxwidth;
	u32 maxblendstages;
	u32 blendstage_base[MAX_BLOCKS];
};

/**
 * struct dpu_dspp_sub_blks: Information of DSPP block
 * @pcc: pixel color correction block
 */
struct dpu_dspp_sub_blks {
	struct dpu_pp_blk pcc;
};

struct dpu_pingpong_sub_blks {
	struct dpu_pp_blk te;
	struct dpu_pp_blk te2;
	struct dpu_pp_blk dither;
};

/**
 * struct dpu_dsc_sub_blks - DSC sub-blks
 * @enc: DSC encoder sub-block
 * @ctl: DSC controller sub-block
 */
struct dpu_dsc_sub_blks {
	struct dpu_dsc_blk enc;
	struct dpu_dsc_blk ctl;
};

/**
 * dpu_clk_ctrl_type - Defines top level clock control signals
 */
enum dpu_clk_ctrl_type {
	DPU_CLK_CTRL_NONE,
	DPU_CLK_CTRL_VIG0,
	DPU_CLK_CTRL_VIG1,
	DPU_CLK_CTRL_VIG2,
	DPU_CLK_CTRL_VIG3,
	DPU_CLK_CTRL_VIG4,
	DPU_CLK_CTRL_RGB0,
	DPU_CLK_CTRL_RGB1,
	DPU_CLK_CTRL_RGB2,
	DPU_CLK_CTRL_RGB3,
	DPU_CLK_CTRL_DMA0,
	DPU_CLK_CTRL_DMA1,
	DPU_CLK_CTRL_DMA2,
	DPU_CLK_CTRL_DMA3,
	DPU_CLK_CTRL_DMA4,
	DPU_CLK_CTRL_DMA5,
	DPU_CLK_CTRL_CURSOR0,
	DPU_CLK_CTRL_CURSOR1,
	DPU_CLK_CTRL_INLINE_ROT0_SSPP,
	DPU_CLK_CTRL_REG_DMA,
	DPU_CLK_CTRL_WB2,
	DPU_CLK_CTRL_MAX,
};

/* struct dpu_clk_ctrl_reg : Clock control register
 * @reg_off:           register offset
 * @bit_off:           bit offset
 */
struct dpu_clk_ctrl_reg {
	u32 reg_off;
	u32 bit_off;
};

/* struct dpu_mdp_cfg : MDP TOP-BLK instance info
 * @id:                index identifying this block
 * @base:              register base offset to mdss
 * @features           bit mask identifying sub-blocks/features
 * @clk_ctrls          clock control register definition
 */
struct dpu_mdp_cfg {
	DPU_HW_BLK_INFO;
	struct dpu_clk_ctrl_reg clk_ctrls[DPU_CLK_CTRL_MAX];
};

/* struct dpu_ctl_cfg : MDP CTL instance info
 * @id:                index identifying this block
 * @base:              register base offset to mdss
 * @features           bit mask identifying sub-blocks/features
 * @intr_start:        interrupt index for CTL_START
 */
struct dpu_ctl_cfg {
	DPU_HW_BLK_INFO;
	s32 intr_start;
};

/**
 * struct dpu_sspp_cfg - information of source pipes
 * @id:                index identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 * @sblk:              SSPP sub-blocks information
 * @xin_id:            bus client identifier
 * @clk_ctrl           clock control identifier
 * @type               sspp type identifier
 */
struct dpu_sspp_cfg {
	DPU_HW_BLK_INFO;
	const struct dpu_sspp_sub_blks *sblk;
	u32 xin_id;
	enum dpu_clk_ctrl_type clk_ctrl;
	u32 type;
};

/**
 * struct dpu_lm_cfg - information of layer mixer blocks
 * @id:                index identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 * @sblk:              LM Sub-blocks information
 * @pingpong:          ID of connected PingPong, PINGPONG_NONE if unsupported
 * @lm_pair:           ID of LM that can be controlled by same CTL
 */
struct dpu_lm_cfg {
	DPU_HW_BLK_INFO;
	const struct dpu_lm_sub_blks *sblk;
	u32 pingpong;
	u32 dspp;
	unsigned long lm_pair;
};

/**
 * struct dpu_dspp_cfg - information of DSPP blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 *                     supported by this block
 * @sblk               sub-blocks information
 */
struct dpu_dspp_cfg  {
	DPU_HW_BLK_INFO;
	const struct dpu_dspp_sub_blks *sblk;
};

/**
 * struct dpu_pingpong_cfg - information of PING-PONG blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 * @intr_done:         index for PINGPONG done interrupt
 * @intr_rdptr:        index for PINGPONG readpointer done interrupt
 * @sblk               sub-blocks information
 */
struct dpu_pingpong_cfg  {
	DPU_HW_BLK_INFO;
	u32 merge_3d;
	s32 intr_done;
	s32 intr_rdptr;
	const struct dpu_pingpong_sub_blks *sblk;
};

/**
 * struct dpu_merge_3d_cfg - information of DSPP blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 *                     supported by this block
 * @sblk               sub-blocks information
 */
struct dpu_merge_3d_cfg  {
	DPU_HW_BLK_INFO;
	const struct dpu_merge_3d_sub_blks *sblk;
};

/**
 * struct dpu_dsc_cfg - information of DSC blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @len:               length of hardware block
 * @features           bit mask identifying sub-blocks/features
 * @sblk:              sub-blocks information
 */
struct dpu_dsc_cfg {
	DPU_HW_BLK_INFO;
	const struct dpu_dsc_sub_blks *sblk;
};

/**
 * struct dpu_intf_cfg - information of timing engine blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 * @type:              Interface type(DSI, DP, HDMI)
 * @controller_id:     Controller Instance ID in case of multiple of intf type
 * @prog_fetch_lines_worst_case	Worst case latency num lines needed to prefetch
 * @intr_underrun:	index for INTF underrun interrupt
 * @intr_vsync:	        index for INTF VSYNC interrupt
 * @intr_tear_rd_ptr:  Index for INTF TEAR_RD_PTR interrupt
 */
struct dpu_intf_cfg  {
	DPU_HW_BLK_INFO;
	u32 type;   /* interface type*/
	u32 controller_id;
	u32 prog_fetch_lines_worst_case;
	s32 intr_underrun;
	s32 intr_vsync;
	s32 intr_tear_rd_ptr;
};

/**
 * struct dpu_wb_cfg - information of writeback blocks
 * @DPU_HW_BLK_INFO:    refer to the description above for DPU_HW_BLK_INFO
 * @vbif_idx:           vbif client index
 * @maxlinewidth:       max line width supported by writeback block
 * @xin_id:             bus client identifier
 * @intr_wb_done:       interrupt index for WB_DONE
 * @format_list:	    list of formats supported by this writeback block
 * @num_formats:	    number of formats supported by this writeback block
 * @clk_ctrl:	        clock control identifier
 */
struct dpu_wb_cfg {
	DPU_HW_BLK_INFO;
	u8 vbif_idx;
	u32 maxlinewidth;
	u32 xin_id;
	s32 intr_wb_done;
	const u32 *format_list;
	u32 num_formats;
	enum dpu_clk_ctrl_type clk_ctrl;
};

/**
 * struct dpu_vbif_dynamic_ot_cfg - dynamic OT setting
 * @pps                pixel per seconds
 * @ot_limit           OT limit to use up to specified pixel per second
 */
struct dpu_vbif_dynamic_ot_cfg {
	u64 pps;
	u32 ot_limit;
};

/**
 * struct dpu_vbif_dynamic_ot_tbl - dynamic OT setting table
 * @count              length of cfg
 * @cfg                pointer to array of configuration settings with
 *                     ascending requirements
 */
struct dpu_vbif_dynamic_ot_tbl {
	u32 count;
	const struct dpu_vbif_dynamic_ot_cfg *cfg;
};

/**
 * struct dpu_vbif_qos_tbl - QoS priority table
 * @npriority_lvl      num of priority level
 * @priority_lvl       pointer to array of priority level in ascending order
 */
struct dpu_vbif_qos_tbl {
	u32 npriority_lvl;
	const u32 *priority_lvl;
};

/**
 * struct dpu_vbif_cfg - information of VBIF blocks
 * @id                 enum identifying this block
 * @base               register offset of this block
 * @features           bit mask identifying sub-blocks/features
 * @ot_rd_limit        default OT read limit
 * @ot_wr_limit        default OT write limit
 * @xin_halt_timeout   maximum time (in usec) for xin to halt
 * @qos_rp_remap_size  size of VBIF_XINL_QOS_RP_REMAP register space
 * @dynamic_ot_rd_tbl  dynamic OT read configuration table
 * @dynamic_ot_wr_tbl  dynamic OT write configuration table
 * @qos_rt_tbl         real-time QoS priority table
 * @qos_nrt_tbl        non-real-time QoS priority table
 * @memtype_count      number of defined memtypes
 * @memtype            array of xin memtype definitions
 */
struct dpu_vbif_cfg {
	DPU_HW_BLK_INFO;
	u32 default_ot_rd_limit;
	u32 default_ot_wr_limit;
	u32 xin_halt_timeout;
	u32 qos_rp_remap_size;
	struct dpu_vbif_dynamic_ot_tbl dynamic_ot_rd_tbl;
	struct dpu_vbif_dynamic_ot_tbl dynamic_ot_wr_tbl;
	struct dpu_vbif_qos_tbl qos_rt_tbl;
	struct dpu_vbif_qos_tbl qos_nrt_tbl;
	u32 memtype_count;
	u32 memtype[MAX_XIN_COUNT];
};

/**
 * Define CDP use cases
 * @DPU_PERF_CDP_UDAGE_RT: real-time use cases
 * @DPU_PERF_CDP_USAGE_NRT: non real-time use cases such as WFD
 */
enum {
	DPU_PERF_CDP_USAGE_RT,
	DPU_PERF_CDP_USAGE_NRT,
	DPU_PERF_CDP_USAGE_MAX
};

/**
 * struct dpu_perf_cdp_cfg - define CDP use case configuration
 * @rd_enable: true if read pipe CDP is enabled
 * @wr_enable: true if write pipe CDP is enabled
 */
struct dpu_perf_cdp_cfg {
	bool rd_enable;
	bool wr_enable;
};

/**
 * struct dpu_mdss_version - DPU's major and minor versions
 * @core_major_ver: DPU core's major version
 * @core_minor_ver: DPU core's minor version
 */
struct dpu_mdss_version {
	u8 core_major_ver;
	u8 core_minor_ver;
};

/**
 * struct dpu_perf_cfg - performance control settings
 * @max_bw_low         low threshold of maximum bandwidth (kbps)
 * @max_bw_high        high threshold of maximum bandwidth (kbps)
 * @min_core_ib        minimum bandwidth for core (kbps)
 * @min_core_ib        minimum mnoc ib vote in kbps
 * @min_llcc_ib        minimum llcc ib vote in kbps
 * @min_dram_ib        minimum dram ib vote in kbps
 * @undersized_prefill_lines   undersized prefill in lines
 * @xtra_prefill_lines         extra prefill latency in lines
 * @dest_scale_prefill_lines   destination scaler latency in lines
 * @macrotile_perfill_lines    macrotile latency in lines
 * @yuv_nv12_prefill_lines     yuv_nv12 latency in lines
 * @linear_prefill_lines       linear latency in lines
 * @downscaling_prefill_lines  downscaling latency in lines
 * @amortizable_theshold minimum y position for traffic shaping prefill
 * @min_prefill_lines  minimum pipeline latency in lines
 * @clk_inefficiency_factor DPU src clock inefficiency factor
 * @bw_inefficiency_factor DPU axi bus bw inefficiency factor
 * @safe_lut_tbl: LUT tables for safe signals
 * @danger_lut_tbl: LUT tables for danger signals
 * @qos_lut_tbl: LUT tables for QoS signals
 * @cdp_cfg            cdp use case configurations
 */
struct dpu_perf_cfg {
	u32 max_bw_low;
	u32 max_bw_high;
	u32 min_core_ib;
	u32 min_llcc_ib;
	u32 min_dram_ib;
	u32 undersized_prefill_lines;
	u32 xtra_prefill_lines;
	u32 dest_scale_prefill_lines;
	u32 macrotile_prefill_lines;
	u32 yuv_nv12_prefill_lines;
	u32 linear_prefill_lines;
	u32 downscaling_prefill_lines;
	u32 amortizable_threshold;
	u32 min_prefill_lines;
	u32 clk_inefficiency_factor;
	u32 bw_inefficiency_factor;
	u32 safe_lut_tbl[DPU_QOS_LUT_USAGE_MAX];
	u32 danger_lut_tbl[DPU_QOS_LUT_USAGE_MAX];
	struct dpu_qos_lut_tbl qos_lut_tbl[DPU_QOS_LUT_USAGE_MAX];
	struct dpu_perf_cdp_cfg cdp_cfg[DPU_PERF_CDP_USAGE_MAX];
};

/**
 * struct dpu_mdss_cfg - information of MDSS HW
 * This is the main catalog data structure representing
 * this HW version. Contains dpu's major and minor versions,
 * number of instances, register offsets, capabilities of the
 * all MDSS HW sub-blocks.
 *
 * @dma_formats        Supported formats for dma pipe
 * @cursor_formats     Supported formats for cursor pipe
 * @vig_formats        Supported formats for vig pipe
 */
struct dpu_mdss_cfg {
	const struct dpu_mdss_version *mdss_ver;

	const struct dpu_caps *caps;

	const struct dpu_mdp_cfg *mdp;

	u32 ctl_count;
	const struct dpu_ctl_cfg *ctl;

	u32 sspp_count;
	const struct dpu_sspp_cfg *sspp;

	u32 mixer_count;
	const struct dpu_lm_cfg *mixer;

	u32 pingpong_count;
	const struct dpu_pingpong_cfg *pingpong;

	u32 merge_3d_count;
	const struct dpu_merge_3d_cfg *merge_3d;

	u32 dsc_count;
	const struct dpu_dsc_cfg *dsc;

	u32 intf_count;
	const struct dpu_intf_cfg *intf;

	u32 vbif_count;
	const struct dpu_vbif_cfg *vbif;

	u32 wb_count;
	const struct dpu_wb_cfg *wb;

	u32 ad_count;

	u32 dspp_count;
	const struct dpu_dspp_cfg *dspp;

	/* Add additional block data structures here */

	const struct dpu_perf_cfg *perf;
	const struct dpu_format_extended *dma_formats;
	const struct dpu_format_extended *cursor_formats;
	const struct dpu_format_extended *vig_formats;
};

extern const struct dpu_mdss_cfg dpu_msm8998_cfg;
extern const struct dpu_mdss_cfg dpu_sdm845_cfg;
extern const struct dpu_mdss_cfg dpu_sm8150_cfg;
extern const struct dpu_mdss_cfg dpu_sc8180x_cfg;
extern const struct dpu_mdss_cfg dpu_sm8250_cfg;
extern const struct dpu_mdss_cfg dpu_sc7180_cfg;
extern const struct dpu_mdss_cfg dpu_sm6115_cfg;
extern const struct dpu_mdss_cfg dpu_sm6125_cfg;
extern const struct dpu_mdss_cfg dpu_sm6350_cfg;
extern const struct dpu_mdss_cfg dpu_qcm2290_cfg;
extern const struct dpu_mdss_cfg dpu_sm6375_cfg;
extern const struct dpu_mdss_cfg dpu_sm8350_cfg;
extern const struct dpu_mdss_cfg dpu_sc7280_cfg;
extern const struct dpu_mdss_cfg dpu_sc8280xp_cfg;
extern const struct dpu_mdss_cfg dpu_sm8450_cfg;
extern const struct dpu_mdss_cfg dpu_sm8550_cfg;

#endif /* _DPU_HW_CATALOG_H */
