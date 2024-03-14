// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VDEC driver
 *
 * Copyright (C) 2021 Collabora Ltd, Emil Velikov <emil.velikov@collabora.com>
 */

#include "hantro.h"

/*
 * Supported formats.
 */

static const struct hantro_fmt sama5d4_vdec_postproc_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.codec_mode = HANTRO_MODE_NONE,
		.postprocessed = true,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_HD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_HD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
};

static const struct hantro_fmt sama5d4_vdec_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_HD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_HD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_MPEG2_SLICE,
		.codec_mode = HANTRO_MODE_MPEG2_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_HD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_HD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_VP8_FRAME,
		.codec_mode = HANTRO_MODE_VP8_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_HD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_HD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_HD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_HD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
};

/*
 * Supported codec ops.
 */

static const struct hantro_codec_ops sama5d4_vdec_codec_ops[] = {
	[HANTRO_MODE_MPEG2_DEC] = {
		.run = hantro_g1_mpeg2_dec_run,
		.reset = hantro_g1_reset,
		.init = hantro_mpeg2_dec_init,
		.exit = hantro_mpeg2_dec_exit,
	},
	[HANTRO_MODE_VP8_DEC] = {
		.run = hantro_g1_vp8_dec_run,
		.reset = hantro_g1_reset,
		.init = hantro_vp8_dec_init,
		.exit = hantro_vp8_dec_exit,
	},
	[HANTRO_MODE_H264_DEC] = {
		.run = hantro_g1_h264_dec_run,
		.reset = hantro_g1_reset,
		.init = hantro_h264_dec_init,
		.exit = hantro_h264_dec_exit,
	},
};

static const struct hantro_irq sama5d4_irqs[] = {
	{ "vdec", hantro_g1_irq },
};

static const char * const sama5d4_clk_names[] = { "vdec_clk" };

const struct hantro_variant sama5d4_vdec_variant = {
	.dec_fmts = sama5d4_vdec_fmts,
	.num_dec_fmts = ARRAY_SIZE(sama5d4_vdec_fmts),
	.postproc_fmts = sama5d4_vdec_postproc_fmts,
	.num_postproc_fmts = ARRAY_SIZE(sama5d4_vdec_postproc_fmts),
	.postproc_ops = &hantro_g1_postproc_ops,
	.codec = HANTRO_MPEG2_DECODER | HANTRO_VP8_DECODER |
		 HANTRO_H264_DECODER,
	.codec_ops = sama5d4_vdec_codec_ops,
	.irqs = sama5d4_irqs,
	.num_irqs = ARRAY_SIZE(sama5d4_irqs),
	.clk_names = sama5d4_clk_names,
	.num_clocks = ARRAY_SIZE(sama5d4_clk_names),
};
