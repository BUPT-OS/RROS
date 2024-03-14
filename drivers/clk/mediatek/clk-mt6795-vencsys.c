// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Collabora Ltd.
 * Author: AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <dt-bindings/clock/mediatek,mt6795-clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs venc_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_VENC(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &venc_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate venc_clks[] = {
	GATE_VENC(CLK_VENC_LARB, "venc_larb", "venc_sel", 0),
	GATE_VENC(CLK_VENC_VENC, "venc_venc", "venc_sel", 4),
	GATE_VENC(CLK_VENC_JPGENC, "venc_jpgenc", "venc_sel", 8),
	GATE_VENC(CLK_VENC_JPGDEC, "venc_jpgdec", "venc_sel", 12),
};

static const struct mtk_clk_desc venc_desc = {
	.clks = venc_clks,
	.num_clks = ARRAY_SIZE(venc_clks),
};

static const struct of_device_id of_match_clk_mt6795_vencsys[] = {
	{ .compatible = "mediatek,mt6795-vencsys", .data = &venc_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6795_vencsys);

static struct platform_driver clk_mt6795_vencsys_drv = {
	.driver = {
		.name = "clk-mt6795-vencsys",
		.of_match_table = of_match_clk_mt6795_vencsys,
	},
	.probe = mtk_clk_simple_probe,
	.remove_new = mtk_clk_simple_remove,
};
module_platform_driver(clk_mt6795_vencsys_drv);

MODULE_DESCRIPTION("MediaTek MT6795 vdecsys clocks driver");
MODULE_LICENSE("GPL");
