// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car Gen4 Clock Pulse Generator
 *
 * Copyright (C) 2021 Renesas Electronics Corp.
 *
 * Based on rcar-gen3-cpg.c
 *
 * Copyright (C) 2015-2018 Glider bvba
 * Copyright (C) 2019 Renesas Electronics Corp.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/slab.h>

#include "renesas-cpg-mssr.h"
#include "rcar-gen4-cpg.h"
#include "rcar-cpg-lib.h"

static const struct rcar_gen4_cpg_pll_config *cpg_pll_config __initdata;
static unsigned int cpg_clk_extalr __initdata;
static u32 cpg_mode __initdata;

#define CPG_PLLECR		0x0820	/* PLL Enable Control Register */

#define CPG_PLLECR_PLLST(n)	BIT(8 + ((n) < 3 ? (n) - 1 : \
					 (n) > 3 ? (n) + 1 : n)) /* PLLn Circuit Status */

#define CPG_PLL1CR0		0x830	/* PLLn Control Registers */
#define CPG_PLL1CR1		0x8b0
#define CPG_PLL2CR0		0x834
#define CPG_PLL2CR1		0x8b8
#define CPG_PLL3CR0		0x83c
#define CPG_PLL3CR1		0x8c0
#define CPG_PLL4CR0		0x844
#define CPG_PLL4CR1		0x8c8
#define CPG_PLL6CR0		0x84c
#define CPG_PLL6CR1		0x8d8

#define CPG_PLLxCR0_KICK	BIT(31)
#define CPG_PLLxCR0_NI		GENMASK(27, 20)	/* Integer mult. factor */
#define CPG_PLLxCR0_SSMODE	GENMASK(18, 16)	/* PLL mode */
#define CPG_PLLxCR0_SSMODE_FM	BIT(18)	/* Fractional Multiplication */
#define CPG_PLLxCR0_SSMODE_DITH	BIT(17) /* Frequency Dithering */
#define CPG_PLLxCR0_SSMODE_CENT	BIT(16)	/* Center (vs. Down) Spread Dithering */
#define CPG_PLLxCR0_SSFREQ	GENMASK(14, 8)	/* SSCG Modulation Frequency */
#define CPG_PLLxCR0_SSDEPT	GENMASK(6, 0)	/* SSCG Modulation Depth */

#define SSMODE_FM		BIT(2)	/* Fractional Multiplication */
#define SSMODE_DITHER		BIT(1)	/* Frequency Dithering */
#define SSMODE_CENTER		BIT(0)	/* Center (vs. Down) Spread Dithering */

/* PLL Clocks */
struct cpg_pll_clk {
	struct clk_hw hw;
	void __iomem *pllcr0_reg;
	void __iomem *pllecr_reg;
	u32 pllecr_pllst_mask;
};

#define to_pll_clk(_hw)   container_of(_hw, struct cpg_pll_clk, hw)

static unsigned long cpg_pll_clk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct cpg_pll_clk *pll_clk = to_pll_clk(hw);
	unsigned int mult;

	mult = FIELD_GET(CPG_PLLxCR0_NI, readl(pll_clk->pllcr0_reg)) + 1;

	return parent_rate * mult * 2;
}

static int cpg_pll_clk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	unsigned int min_mult, max_mult, mult;
	unsigned long prate;

	prate = req->best_parent_rate * 2;
	min_mult = max(div64_ul(req->min_rate, prate), 1ULL);
	max_mult = min(div64_ul(req->max_rate, prate), 256ULL);
	if (max_mult < min_mult)
		return -EINVAL;

	mult = DIV_ROUND_CLOSEST_ULL(req->rate, prate);
	mult = clamp(mult, min_mult, max_mult);

	req->rate = prate * mult;
	return 0;
}

static int cpg_pll_clk_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct cpg_pll_clk *pll_clk = to_pll_clk(hw);
	unsigned int mult;
	u32 val;

	mult = DIV_ROUND_CLOSEST_ULL(rate, parent_rate * 2);
	mult = clamp(mult, 1U, 256U);

	if (readl(pll_clk->pllcr0_reg) & CPG_PLLxCR0_KICK)
		return -EBUSY;

	cpg_reg_modify(pll_clk->pllcr0_reg, CPG_PLLxCR0_NI,
		       FIELD_PREP(CPG_PLLxCR0_NI, mult - 1));

	/*
	 * Set KICK bit in PLLxCR0 to update hardware setting and wait for
	 * clock change completion.
	 */
	cpg_reg_modify(pll_clk->pllcr0_reg, 0, CPG_PLLxCR0_KICK);

	/*
	 * Note: There is no HW information about the worst case latency.
	 *
	 * Using experimental measurements, it seems that no more than
	 * ~45 µs are needed, independently of the CPU rate.
	 * Since this value might be dependent on external xtal rate, pll
	 * rate or even the other emulation clocks rate, use 1000 as a
	 * "super" safe value.
	 */
	return readl_poll_timeout(pll_clk->pllecr_reg, val,
				  val & pll_clk->pllecr_pllst_mask, 0, 1000);
}

static const struct clk_ops cpg_pll_clk_ops = {
	.recalc_rate = cpg_pll_clk_recalc_rate,
	.determine_rate = cpg_pll_clk_determine_rate,
	.set_rate = cpg_pll_clk_set_rate,
};

static struct clk * __init cpg_pll_clk_register(const char *name,
						const char *parent_name,
						void __iomem *base,
						unsigned int cr0_offset,
						unsigned int cr1_offset,
						unsigned int index)

{
	struct cpg_pll_clk *pll_clk;
	struct clk_init_data init = {};
	struct clk *clk;

	pll_clk = kzalloc(sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &cpg_pll_clk_ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	pll_clk->hw.init = &init;
	pll_clk->pllcr0_reg = base + cr0_offset;
	pll_clk->pllecr_reg = base + CPG_PLLECR;
	pll_clk->pllecr_pllst_mask = CPG_PLLECR_PLLST(index);

	/* Disable Fractional Multiplication and Frequency Dithering */
	writel(0, base + cr1_offset);
	cpg_reg_modify(pll_clk->pllcr0_reg, CPG_PLLxCR0_SSMODE, 0);

	clk = clk_register(NULL, &pll_clk->hw);
	if (IS_ERR(clk))
		kfree(pll_clk);

	return clk;
}
/*
 * Z0 Clock & Z1 Clock
 */
#define CPG_FRQCRB			0x00000804
#define CPG_FRQCRB_KICK			BIT(31)
#define CPG_FRQCRC			0x00000808

struct cpg_z_clk {
	struct clk_hw hw;
	void __iomem *reg;
	void __iomem *kick_reg;
	unsigned long max_rate;		/* Maximum rate for normal mode */
	unsigned int fixed_div;
	u32 mask;
};

#define to_z_clk(_hw)	container_of(_hw, struct cpg_z_clk, hw)

static unsigned long cpg_z_clk_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct cpg_z_clk *zclk = to_z_clk(hw);
	unsigned int mult;
	u32 val;

	val = readl(zclk->reg) & zclk->mask;
	mult = 32 - (val >> __ffs(zclk->mask));

	return DIV_ROUND_CLOSEST_ULL((u64)parent_rate * mult,
				     32 * zclk->fixed_div);
}

static int cpg_z_clk_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	struct cpg_z_clk *zclk = to_z_clk(hw);
	unsigned int min_mult, max_mult, mult;
	unsigned long rate, prate;

	rate = min(req->rate, req->max_rate);
	if (rate <= zclk->max_rate) {
		/* Set parent rate to initial value for normal modes */
		prate = zclk->max_rate;
	} else {
		/* Set increased parent rate for boost modes */
		prate = rate;
	}
	req->best_parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw),
						  prate * zclk->fixed_div);

	prate = req->best_parent_rate / zclk->fixed_div;
	min_mult = max(div64_ul(req->min_rate * 32ULL, prate), 1ULL);
	max_mult = min(div64_ul(req->max_rate * 32ULL, prate), 32ULL);
	if (max_mult < min_mult)
		return -EINVAL;

	mult = DIV_ROUND_CLOSEST_ULL(rate * 32ULL, prate);
	mult = clamp(mult, min_mult, max_mult);

	req->rate = DIV_ROUND_CLOSEST_ULL((u64)prate * mult, 32);
	return 0;
}

static int cpg_z_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	struct cpg_z_clk *zclk = to_z_clk(hw);
	unsigned int mult;
	unsigned int i;

	mult = DIV64_U64_ROUND_CLOSEST(rate * 32ULL * zclk->fixed_div,
				       parent_rate);
	mult = clamp(mult, 1U, 32U);

	if (readl(zclk->kick_reg) & CPG_FRQCRB_KICK)
		return -EBUSY;

	cpg_reg_modify(zclk->reg, zclk->mask, (32 - mult) << __ffs(zclk->mask));

	/*
	 * Set KICK bit in FRQCRB to update hardware setting and wait for
	 * clock change completion.
	 */
	cpg_reg_modify(zclk->kick_reg, 0, CPG_FRQCRB_KICK);

	/*
	 * Note: There is no HW information about the worst case latency.
	 *
	 * Using experimental measurements, it seems that no more than
	 * ~10 iterations are needed, independently of the CPU rate.
	 * Since this value might be dependent on external xtal rate, pll1
	 * rate or even the other emulation clocks rate, use 1000 as a
	 * "super" safe value.
	 */
	for (i = 1000; i; i--) {
		if (!(readl(zclk->kick_reg) & CPG_FRQCRB_KICK))
			return 0;

		cpu_relax();
	}

	return -ETIMEDOUT;
}

static const struct clk_ops cpg_z_clk_ops = {
	.recalc_rate = cpg_z_clk_recalc_rate,
	.determine_rate = cpg_z_clk_determine_rate,
	.set_rate = cpg_z_clk_set_rate,
};

static struct clk * __init cpg_z_clk_register(const char *name,
					      const char *parent_name,
					      void __iomem *reg,
					      unsigned int div,
					      unsigned int offset)
{
	struct clk_init_data init = {};
	struct cpg_z_clk *zclk;
	struct clk *clk;

	zclk = kzalloc(sizeof(*zclk), GFP_KERNEL);
	if (!zclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &cpg_z_clk_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	zclk->reg = reg + CPG_FRQCRC;
	zclk->kick_reg = reg + CPG_FRQCRB;
	zclk->hw.init = &init;
	zclk->mask = GENMASK(offset + 4, offset);
	zclk->fixed_div = div; /* PLLVCO x 1/div x SYS-CPU divider */

	clk = clk_register(NULL, &zclk->hw);
	if (IS_ERR(clk)) {
		kfree(zclk);
		return clk;
	}

	zclk->max_rate = clk_hw_get_rate(clk_hw_get_parent(&zclk->hw)) /
			 zclk->fixed_div;
	return clk;
}

/*
 * RPC Clocks
 */
static const struct clk_div_table cpg_rpcsrc_div_table[] = {
	{ 0, 4 }, { 1, 6 }, { 2, 5 }, { 3, 6 }, { 0, 0 },
};

struct clk * __init rcar_gen4_cpg_clk_register(struct device *dev,
	const struct cpg_core_clk *core, const struct cpg_mssr_info *info,
	struct clk **clks, void __iomem *base,
	struct raw_notifier_head *notifiers)
{
	const struct clk *parent;
	unsigned int mult = 1;
	unsigned int div = 1;
	u32 value;

	parent = clks[core->parent & 0xffff];	/* some types use high bits */
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	switch (core->type) {
	case CLK_TYPE_GEN4_MAIN:
		div = cpg_pll_config->extal_div;
		break;

	case CLK_TYPE_GEN4_PLL1:
		mult = cpg_pll_config->pll1_mult;
		div = cpg_pll_config->pll1_div;
		break;

	case CLK_TYPE_GEN4_PLL2_VAR:
		/*
		 * PLL2 is implemented as a custom clock, to change the
		 * multiplier when cpufreq changes between normal and boost
		 * modes.
		 */
		return cpg_pll_clk_register(core->name, __clk_get_name(parent),
					    base, CPG_PLL2CR0, CPG_PLL2CR1, 2);

	case CLK_TYPE_GEN4_PLL2:
		mult = cpg_pll_config->pll2_mult;
		div = cpg_pll_config->pll2_div;
		break;

	case CLK_TYPE_GEN4_PLL3:
		mult = cpg_pll_config->pll3_mult;
		div = cpg_pll_config->pll3_div;
		break;

	case CLK_TYPE_GEN4_PLL4:
		mult = cpg_pll_config->pll4_mult;
		div = cpg_pll_config->pll4_div;
		break;

	case CLK_TYPE_GEN4_PLL5:
		mult = cpg_pll_config->pll5_mult;
		div = cpg_pll_config->pll5_div;
		break;

	case CLK_TYPE_GEN4_PLL6:
		mult = cpg_pll_config->pll6_mult;
		div = cpg_pll_config->pll6_div;
		break;

	case CLK_TYPE_GEN4_PLL2X_3X:
		value = readl(base + core->offset);
		mult = (((value >> 24) & 0x7f) + 1) * 2;
		break;

	case CLK_TYPE_GEN4_Z:
		return cpg_z_clk_register(core->name, __clk_get_name(parent),
					  base, core->div, core->offset);

	case CLK_TYPE_GEN4_SDSRC:
		div = ((readl(base + SD0CKCR1) >> 29) & 0x03) + 4;
		break;

	case CLK_TYPE_GEN4_SDH:
		return cpg_sdh_clk_register(core->name, base + core->offset,
					   __clk_get_name(parent), notifiers);

	case CLK_TYPE_GEN4_SD:
		return cpg_sd_clk_register(core->name, base + core->offset,
					   __clk_get_name(parent));

	case CLK_TYPE_GEN4_MDSEL:
		/*
		 * Clock selectable between two parents and two fixed dividers
		 * using a mode pin
		 */
		if (cpg_mode & BIT(core->offset)) {
			div = core->div & 0xffff;
		} else {
			parent = clks[core->parent >> 16];
			if (IS_ERR(parent))
				return ERR_CAST(parent);
			div = core->div >> 16;
		}
		mult = 1;
		break;

	case CLK_TYPE_GEN4_OSC:
		/*
		 * Clock combining OSC EXTAL predivider and a fixed divider
		 */
		div = cpg_pll_config->osc_prediv * core->div;
		break;

	case CLK_TYPE_GEN4_RPCSRC:
		return clk_register_divider_table(NULL, core->name,
						  __clk_get_name(parent), 0,
						  base + CPG_RPCCKCR, 3, 2, 0,
						  cpg_rpcsrc_div_table,
						  &cpg_lock);

	case CLK_TYPE_GEN4_RPC:
		return cpg_rpc_clk_register(core->name, base + CPG_RPCCKCR,
					    __clk_get_name(parent), notifiers);

	case CLK_TYPE_GEN4_RPCD2:
		return cpg_rpcd2_clk_register(core->name, base + CPG_RPCCKCR,
					      __clk_get_name(parent));

	default:
		return ERR_PTR(-EINVAL);
	}

	return clk_register_fixed_factor(NULL, core->name,
					 __clk_get_name(parent), 0, mult, div);
}

int __init rcar_gen4_cpg_init(const struct rcar_gen4_cpg_pll_config *config,
			      unsigned int clk_extalr, u32 mode)
{
	cpg_pll_config = config;
	cpg_clk_extalr = clk_extalr;
	cpg_mode = mode;

	spin_lock_init(&cpg_lock);

	return 0;
}
