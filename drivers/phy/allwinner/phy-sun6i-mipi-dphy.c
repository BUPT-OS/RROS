// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2016 Allwinnertech Co., Ltd.
 * Copyright (C) 2017-2018 Bootlin
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>

#define SUN6I_DPHY_GCTL_REG		0x00
#define SUN6I_DPHY_GCTL_LANE_NUM(n)		((((n) - 1) & 3) << 4)
#define SUN6I_DPHY_GCTL_EN			BIT(0)

#define SUN6I_DPHY_TX_CTL_REG		0x04
#define SUN6I_DPHY_TX_CTL_HS_TX_CLK_CONT	BIT(28)

#define SUN6I_DPHY_RX_CTL_REG		0x08
#define SUN6I_DPHY_RX_CTL_EN_DBC	BIT(31)
#define SUN6I_DPHY_RX_CTL_RX_CLK_FORCE	BIT(24)
#define SUN6I_DPHY_RX_CTL_RX_D3_FORCE	BIT(23)
#define SUN6I_DPHY_RX_CTL_RX_D2_FORCE	BIT(22)
#define SUN6I_DPHY_RX_CTL_RX_D1_FORCE	BIT(21)
#define SUN6I_DPHY_RX_CTL_RX_D0_FORCE	BIT(20)

#define SUN6I_DPHY_TX_TIME0_REG		0x10
#define SUN6I_DPHY_TX_TIME0_HS_TRAIL(n)		(((n) & 0xff) << 24)
#define SUN6I_DPHY_TX_TIME0_HS_PREPARE(n)	(((n) & 0xff) << 16)
#define SUN6I_DPHY_TX_TIME0_LP_CLK_DIV(n)	((n) & 0xff)

#define SUN6I_DPHY_TX_TIME1_REG		0x14
#define SUN6I_DPHY_TX_TIME1_CLK_POST(n)		(((n) & 0xff) << 24)
#define SUN6I_DPHY_TX_TIME1_CLK_PRE(n)		(((n) & 0xff) << 16)
#define SUN6I_DPHY_TX_TIME1_CLK_ZERO(n)		(((n) & 0xff) << 8)
#define SUN6I_DPHY_TX_TIME1_CLK_PREPARE(n)	((n) & 0xff)

#define SUN6I_DPHY_TX_TIME2_REG		0x18
#define SUN6I_DPHY_TX_TIME2_CLK_TRAIL(n)	((n) & 0xff)

#define SUN6I_DPHY_TX_TIME3_REG		0x1c

#define SUN6I_DPHY_TX_TIME4_REG		0x20
#define SUN6I_DPHY_TX_TIME4_HS_TX_ANA1(n)	(((n) & 0xff) << 8)
#define SUN6I_DPHY_TX_TIME4_HS_TX_ANA0(n)	((n) & 0xff)

#define SUN6I_DPHY_RX_TIME0_REG		0x30
#define SUN6I_DPHY_RX_TIME0_HS_RX_SYNC(n)	(((n) & 0xff) << 24)
#define SUN6I_DPHY_RX_TIME0_HS_RX_CLK_MISS(n)	(((n) & 0xff) << 16)
#define SUN6I_DPHY_RX_TIME0_LP_RX(n)		(((n) & 0xff) << 8)

#define SUN6I_DPHY_RX_TIME1_REG		0x34
#define SUN6I_DPHY_RX_TIME1_RX_DLY(n)		(((n) & 0xfff) << 20)
#define SUN6I_DPHY_RX_TIME1_LP_RX_ULPS_WP(n)	((n) & 0xfffff)

#define SUN6I_DPHY_RX_TIME2_REG		0x38
#define SUN6I_DPHY_RX_TIME2_HS_RX_ANA1(n)	(((n) & 0xff) << 8)
#define SUN6I_DPHY_RX_TIME2_HS_RX_ANA0(n)	((n) & 0xff)

#define SUN6I_DPHY_RX_TIME3_REG		0x40
#define SUN6I_DPHY_RX_TIME3_LPRST_DLY(n)	(((n) & 0xffff) << 16)

#define SUN6I_DPHY_ANA0_REG		0x4c
#define SUN6I_DPHY_ANA0_REG_PWS			BIT(31)
#define SUN6I_DPHY_ANA0_REG_PWEND		BIT(30)
#define SUN6I_DPHY_ANA0_REG_PWENC		BIT(29)
#define SUN6I_DPHY_ANA0_REG_DMPC		BIT(28)
#define SUN6I_DPHY_ANA0_REG_DMPD(n)		(((n) & 0xf) << 24)
#define SUN6I_DPHY_ANA0_REG_SRXDT(n)		(((n) & 0xf) << 20)
#define SUN6I_DPHY_ANA0_REG_SRXCK(n)		(((n) & 0xf) << 16)
#define SUN6I_DPHY_ANA0_REG_SDIV2		BIT(15)
#define SUN6I_DPHY_ANA0_REG_SLV(n)		(((n) & 7) << 12)
#define SUN6I_DPHY_ANA0_REG_DEN(n)		(((n) & 0xf) << 8)
#define SUN6I_DPHY_ANA0_REG_PLR(n)		(((n) & 0xf) << 4)
#define SUN6I_DPHY_ANA0_REG_SFB(n)		(((n) & 3) << 2)
#define SUN6I_DPHY_ANA0_REG_RSD			BIT(1)
#define SUN6I_DPHY_ANA0_REG_SELSCK		BIT(0)

#define SUN6I_DPHY_ANA1_REG		0x50
#define SUN6I_DPHY_ANA1_REG_VTTMODE		BIT(31)
#define SUN6I_DPHY_ANA1_REG_CSMPS(n)		(((n) & 3) << 28)
#define SUN6I_DPHY_ANA1_REG_SVTT(n)		(((n) & 0xf) << 24)

#define SUN6I_DPHY_ANA2_REG		0x54
#define SUN6I_DPHY_ANA2_EN_P2S_CPU(n)		(((n) & 0xf) << 24)
#define SUN6I_DPHY_ANA2_EN_P2S_CPU_MASK		GENMASK(27, 24)
#define SUN6I_DPHY_ANA2_EN_CK_CPU		BIT(4)
#define SUN6I_DPHY_ANA2_REG_ENIB		BIT(1)

#define SUN6I_DPHY_ANA3_REG		0x58
#define SUN6I_DPHY_ANA3_EN_VTTD(n)		(((n) & 0xf) << 28)
#define SUN6I_DPHY_ANA3_EN_VTTD_MASK		GENMASK(31, 28)
#define SUN6I_DPHY_ANA3_EN_VTTC			BIT(27)
#define SUN6I_DPHY_ANA3_EN_DIV			BIT(26)
#define SUN6I_DPHY_ANA3_EN_LDOC			BIT(25)
#define SUN6I_DPHY_ANA3_EN_LDOD			BIT(24)
#define SUN6I_DPHY_ANA3_EN_LDOR			BIT(18)

#define SUN6I_DPHY_ANA4_REG		0x5c
#define SUN6I_DPHY_ANA4_REG_EN_MIPI		BIT(31)
#define SUN6I_DPHY_ANA4_REG_EN_COMTEST		BIT(30)
#define SUN6I_DPHY_ANA4_REG_COMTEST(n)		(((n) & 3) << 28)
#define SUN6I_DPHY_ANA4_REG_IB(n)		(((n) & 3) << 25)
#define SUN6I_DPHY_ANA4_REG_DMPLVC		BIT(24)
#define SUN6I_DPHY_ANA4_REG_DMPLVD(n)		(((n) & 0xf) << 20)
#define SUN6I_DPHY_ANA4_REG_VTT_SET(n)		(((n) & 0x7) << 17)
#define SUN6I_DPHY_ANA4_REG_CKDV(n)		(((n) & 0x1f) << 12)
#define SUN6I_DPHY_ANA4_REG_TMSC(n)		(((n) & 3) << 10)
#define SUN6I_DPHY_ANA4_REG_TMSD(n)		(((n) & 3) << 8)
#define SUN6I_DPHY_ANA4_REG_TXDNSC(n)		(((n) & 3) << 6)
#define SUN6I_DPHY_ANA4_REG_TXDNSD(n)		(((n) & 3) << 4)
#define SUN6I_DPHY_ANA4_REG_TXPUSC(n)		(((n) & 3) << 2)
#define SUN6I_DPHY_ANA4_REG_TXPUSD(n)		((n) & 3)

#define SUN6I_DPHY_DBG5_REG		0xf4

#define SUN50I_DPHY_TX_SLEW_REG0	0xf8
#define SUN50I_DPHY_TX_SLEW_REG1	0xfc
#define SUN50I_DPHY_TX_SLEW_REG2	0x100

#define SUN50I_DPHY_PLL_REG0		0x104
#define SUN50I_DPHY_PLL_REG0_CP36_EN		BIT(23)
#define SUN50I_DPHY_PLL_REG0_LDO_EN		BIT(22)
#define SUN50I_DPHY_PLL_REG0_EN_LVS		BIT(21)
#define SUN50I_DPHY_PLL_REG0_PLL_EN		BIT(20)
#define SUN50I_DPHY_PLL_REG0_P(n)		(((n) & 0xf) << 16)
#define SUN50I_DPHY_PLL_REG0_N(n)		(((n) & 0xff) << 8)
#define SUN50I_DPHY_PLL_REG0_NDET		BIT(7)
#define SUN50I_DPHY_PLL_REG0_TDIV		BIT(6)
#define SUN50I_DPHY_PLL_REG0_M0(n)		(((n) & 3) << 4)
#define SUN50I_DPHY_PLL_REG0_M1(n)		((n) & 0xf)

#define SUN50I_DPHY_PLL_REG1		0x108
#define SUN50I_DPHY_PLL_REG1_UNLOCK_MDSEL(n)	(((n) & 3) << 14)
#define SUN50I_DPHY_PLL_REG1_LOCKMDSEL		BIT(13)
#define SUN50I_DPHY_PLL_REG1_LOCKDET_EN		BIT(12)
#define SUN50I_DPHY_PLL_REG1_VSETA(n)		(((n) & 0x7) << 9)
#define SUN50I_DPHY_PLL_REG1_VSETD(n)		(((n) & 0x7) << 6)
#define SUN50I_DPHY_PLL_REG1_LPF_SW		BIT(5)
#define SUN50I_DPHY_PLL_REG1_ICP_SEL(n)		(((n) & 3) << 3)
#define SUN50I_DPHY_PLL_REG1_ATEST_SEL(n)	(((n) & 3) << 1)
#define SUN50I_DPHY_PLL_REG1_TEST_EN		BIT(0)

#define SUN50I_DPHY_PLL_REG2		0x10c
#define SUN50I_DPHY_PLL_REG2_SDM_EN		BIT(31)
#define SUN50I_DPHY_PLL_REG2_FF_EN		BIT(30)
#define SUN50I_DPHY_PLL_REG2_SS_EN		BIT(29)
#define SUN50I_DPHY_PLL_REG2_SS_FRAC(n)		(((n) & 0x1ff) << 20)
#define SUN50I_DPHY_PLL_REG2_SS_INT(n)		(((n) & 0xff) << 12)
#define SUN50I_DPHY_PLL_REG2_FRAC(n)		((n) & 0xfff)

#define SUN50I_COMBO_PHY_REG0		0x110
#define SUN50I_COMBO_PHY_REG0_EN_TEST_COMBOLDO	BIT(5)
#define SUN50I_COMBO_PHY_REG0_EN_TEST_0P8	BIT(4)
#define SUN50I_COMBO_PHY_REG0_EN_MIPI		BIT(3)
#define SUN50I_COMBO_PHY_REG0_EN_LVDS		BIT(2)
#define SUN50I_COMBO_PHY_REG0_EN_COMBOLDO	BIT(1)
#define SUN50I_COMBO_PHY_REG0_EN_CP		BIT(0)

#define SUN50I_COMBO_PHY_REG1		0x114
#define SUN50I_COMBO_PHY_REG2_REG_VREF1P6(n)	(((n) & 0x7) << 4)
#define SUN50I_COMBO_PHY_REG2_REG_VREF0P8(n)	((n) & 0x7)

#define SUN50I_COMBO_PHY_REG2		0x118
#define SUN50I_COMBO_PHY_REG2_HS_STOP_DLY(n)	((n) & 0xff)

enum sun6i_dphy_direction {
	SUN6I_DPHY_DIRECTION_TX,
	SUN6I_DPHY_DIRECTION_RX,
};

struct sun6i_dphy;

struct sun6i_dphy_variant {
	void	(*tx_power_on)(struct sun6i_dphy *dphy);
	bool	rx_supported;
};

struct sun6i_dphy {
	struct clk				*bus_clk;
	struct clk				*mod_clk;
	struct regmap				*regs;
	struct reset_control			*reset;

	struct phy				*phy;
	struct phy_configure_opts_mipi_dphy	config;

	const struct sun6i_dphy_variant		*variant;
	enum sun6i_dphy_direction		direction;
};

static int sun6i_dphy_init(struct phy *phy)
{
	struct sun6i_dphy *dphy = phy_get_drvdata(phy);

	reset_control_deassert(dphy->reset);
	clk_prepare_enable(dphy->mod_clk);
	clk_set_rate_exclusive(dphy->mod_clk, 150000000);

	return 0;
}

static int sun6i_dphy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	struct sun6i_dphy *dphy = phy_get_drvdata(phy);
	int ret;

	ret = phy_mipi_dphy_config_validate(&opts->mipi_dphy);
	if (ret)
		return ret;

	memcpy(&dphy->config, opts, sizeof(dphy->config));

	return 0;
}

static void sun6i_a31_mipi_dphy_tx_power_on(struct sun6i_dphy *dphy)
{
	u8 lanes_mask = GENMASK(dphy->config.lanes - 1, 0);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA0_REG,
		     SUN6I_DPHY_ANA0_REG_PWS |
		     SUN6I_DPHY_ANA0_REG_DMPC |
		     SUN6I_DPHY_ANA0_REG_SLV(7) |
		     SUN6I_DPHY_ANA0_REG_DMPD(lanes_mask) |
		     SUN6I_DPHY_ANA0_REG_DEN(lanes_mask));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA1_REG,
		     SUN6I_DPHY_ANA1_REG_CSMPS(1) |
		     SUN6I_DPHY_ANA1_REG_SVTT(7));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA4_REG,
		     SUN6I_DPHY_ANA4_REG_CKDV(1) |
		     SUN6I_DPHY_ANA4_REG_TMSC(1) |
		     SUN6I_DPHY_ANA4_REG_TMSD(1) |
		     SUN6I_DPHY_ANA4_REG_TXDNSC(1) |
		     SUN6I_DPHY_ANA4_REG_TXDNSD(1) |
		     SUN6I_DPHY_ANA4_REG_TXPUSC(1) |
		     SUN6I_DPHY_ANA4_REG_TXPUSD(1) |
		     SUN6I_DPHY_ANA4_REG_DMPLVC |
		     SUN6I_DPHY_ANA4_REG_DMPLVD(lanes_mask));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA2_REG,
		     SUN6I_DPHY_ANA2_REG_ENIB);
	udelay(5);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA3_REG,
		     SUN6I_DPHY_ANA3_EN_LDOR |
		     SUN6I_DPHY_ANA3_EN_LDOC |
		     SUN6I_DPHY_ANA3_EN_LDOD);
	udelay(1);
}

static void sun50i_a100_mipi_dphy_tx_power_on(struct sun6i_dphy *dphy)
{
	unsigned long mipi_symbol_rate = dphy->config.hs_clk_rate;
	unsigned int div, n;

	regmap_write(dphy->regs, SUN6I_DPHY_ANA4_REG,
		     SUN6I_DPHY_ANA4_REG_IB(2) |
		     SUN6I_DPHY_ANA4_REG_DMPLVD(4) |
		     SUN6I_DPHY_ANA4_REG_VTT_SET(3) |
		     SUN6I_DPHY_ANA4_REG_CKDV(3) |
		     SUN6I_DPHY_ANA4_REG_TMSD(1) |
		     SUN6I_DPHY_ANA4_REG_TMSC(1) |
		     SUN6I_DPHY_ANA4_REG_TXPUSD(2) |
		     SUN6I_DPHY_ANA4_REG_TXPUSC(3) |
		     SUN6I_DPHY_ANA4_REG_TXDNSD(2) |
		     SUN6I_DPHY_ANA4_REG_TXDNSC(3));

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA2_REG,
			   SUN6I_DPHY_ANA2_EN_CK_CPU,
			   SUN6I_DPHY_ANA2_EN_CK_CPU);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA2_REG,
			   SUN6I_DPHY_ANA2_REG_ENIB,
			   SUN6I_DPHY_ANA2_REG_ENIB);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA3_REG,
		     SUN6I_DPHY_ANA3_EN_LDOR |
		     SUN6I_DPHY_ANA3_EN_LDOC |
		     SUN6I_DPHY_ANA3_EN_LDOD);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA0_REG,
		     SUN6I_DPHY_ANA0_REG_PLR(4) |
		     SUN6I_DPHY_ANA0_REG_SFB(1));

	regmap_write(dphy->regs, SUN50I_COMBO_PHY_REG0,
		     SUN50I_COMBO_PHY_REG0_EN_CP);

	/* Choose a divider to limit the VCO frequency to around 2 GHz. */
	div = 16 >> order_base_2(DIV_ROUND_UP(mipi_symbol_rate, 264000000));
	n = mipi_symbol_rate * div / 24000000;

	regmap_write(dphy->regs, SUN50I_DPHY_PLL_REG0,
		     SUN50I_DPHY_PLL_REG0_CP36_EN |
		     SUN50I_DPHY_PLL_REG0_LDO_EN |
		     SUN50I_DPHY_PLL_REG0_EN_LVS |
		     SUN50I_DPHY_PLL_REG0_PLL_EN |
		     SUN50I_DPHY_PLL_REG0_NDET |
		     SUN50I_DPHY_PLL_REG0_P((div - 1) % 8) |
		     SUN50I_DPHY_PLL_REG0_N(n) |
		     SUN50I_DPHY_PLL_REG0_M0((div - 1) / 8) |
		     SUN50I_DPHY_PLL_REG0_M1(2));

	/* Disable sigma-delta modulation. */
	regmap_write(dphy->regs, SUN50I_DPHY_PLL_REG2, 0);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA4_REG,
			   SUN6I_DPHY_ANA4_REG_EN_MIPI,
			   SUN6I_DPHY_ANA4_REG_EN_MIPI);

	regmap_update_bits(dphy->regs, SUN50I_COMBO_PHY_REG0,
			   SUN50I_COMBO_PHY_REG0_EN_MIPI |
			   SUN50I_COMBO_PHY_REG0_EN_COMBOLDO,
			   SUN50I_COMBO_PHY_REG0_EN_MIPI |
			   SUN50I_COMBO_PHY_REG0_EN_COMBOLDO);

	regmap_write(dphy->regs, SUN50I_COMBO_PHY_REG2,
		     SUN50I_COMBO_PHY_REG2_HS_STOP_DLY(20));
	udelay(1);
}

static int sun6i_dphy_tx_power_on(struct sun6i_dphy *dphy)
{
	u8 lanes_mask = GENMASK(dphy->config.lanes - 1, 0);

	regmap_write(dphy->regs, SUN6I_DPHY_TX_CTL_REG,
		     SUN6I_DPHY_TX_CTL_HS_TX_CLK_CONT);

	regmap_write(dphy->regs, SUN6I_DPHY_TX_TIME0_REG,
		     SUN6I_DPHY_TX_TIME0_LP_CLK_DIV(14) |
		     SUN6I_DPHY_TX_TIME0_HS_PREPARE(6) |
		     SUN6I_DPHY_TX_TIME0_HS_TRAIL(10));

	regmap_write(dphy->regs, SUN6I_DPHY_TX_TIME1_REG,
		     SUN6I_DPHY_TX_TIME1_CLK_PREPARE(7) |
		     SUN6I_DPHY_TX_TIME1_CLK_ZERO(50) |
		     SUN6I_DPHY_TX_TIME1_CLK_PRE(3) |
		     SUN6I_DPHY_TX_TIME1_CLK_POST(10));

	regmap_write(dphy->regs, SUN6I_DPHY_TX_TIME2_REG,
		     SUN6I_DPHY_TX_TIME2_CLK_TRAIL(30));

	regmap_write(dphy->regs, SUN6I_DPHY_TX_TIME3_REG, 0);

	regmap_write(dphy->regs, SUN6I_DPHY_TX_TIME4_REG,
		     SUN6I_DPHY_TX_TIME4_HS_TX_ANA0(3) |
		     SUN6I_DPHY_TX_TIME4_HS_TX_ANA1(3));

	dphy->variant->tx_power_on(dphy);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA3_REG,
			   SUN6I_DPHY_ANA3_EN_VTTC |
			   SUN6I_DPHY_ANA3_EN_VTTD_MASK,
			   SUN6I_DPHY_ANA3_EN_VTTC |
			   SUN6I_DPHY_ANA3_EN_VTTD(lanes_mask));
	udelay(1);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA3_REG,
			   SUN6I_DPHY_ANA3_EN_DIV,
			   SUN6I_DPHY_ANA3_EN_DIV);
	udelay(1);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA2_REG,
			   SUN6I_DPHY_ANA2_EN_CK_CPU,
			   SUN6I_DPHY_ANA2_EN_CK_CPU);
	udelay(1);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA1_REG,
			   SUN6I_DPHY_ANA1_REG_VTTMODE,
			   SUN6I_DPHY_ANA1_REG_VTTMODE);

	regmap_update_bits(dphy->regs, SUN6I_DPHY_ANA2_REG,
			   SUN6I_DPHY_ANA2_EN_P2S_CPU_MASK,
			   SUN6I_DPHY_ANA2_EN_P2S_CPU(lanes_mask));

	regmap_write(dphy->regs, SUN6I_DPHY_GCTL_REG,
		     SUN6I_DPHY_GCTL_LANE_NUM(dphy->config.lanes) |
		     SUN6I_DPHY_GCTL_EN);

	return 0;
}

static int sun6i_dphy_rx_power_on(struct sun6i_dphy *dphy)
{
	/* Physical clock rate is actually half of symbol rate with DDR. */
	unsigned long mipi_symbol_rate = dphy->config.hs_clk_rate;
	unsigned long dphy_clk_rate;
	unsigned int rx_dly;
	unsigned int lprst_dly;
	u32 value;

	dphy_clk_rate = clk_get_rate(dphy->mod_clk);
	if (!dphy_clk_rate)
		return -EINVAL;

	/* Hardcoded timing parameters from the Allwinner BSP. */
	regmap_write(dphy->regs, SUN6I_DPHY_RX_TIME0_REG,
		     SUN6I_DPHY_RX_TIME0_HS_RX_SYNC(255) |
		     SUN6I_DPHY_RX_TIME0_HS_RX_CLK_MISS(255) |
		     SUN6I_DPHY_RX_TIME0_LP_RX(255));

	/*
	 * Formula from the Allwinner BSP, with hardcoded coefficients
	 * (probably internal divider/multiplier).
	 */
	rx_dly = 8 * (unsigned int)(dphy_clk_rate / (mipi_symbol_rate / 8));

	/*
	 * The Allwinner BSP has an alternative formula for LP_RX_ULPS_WP:
	 * lp_ulps_wp_cnt = lp_ulps_wp_ms * lp_clk / 1000
	 * but does not use it and hardcodes 255 instead.
	 */
	regmap_write(dphy->regs, SUN6I_DPHY_RX_TIME1_REG,
		     SUN6I_DPHY_RX_TIME1_RX_DLY(rx_dly) |
		     SUN6I_DPHY_RX_TIME1_LP_RX_ULPS_WP(255));

	/* HS_RX_ANA0 value is hardcoded in the Allwinner BSP. */
	regmap_write(dphy->regs, SUN6I_DPHY_RX_TIME2_REG,
		     SUN6I_DPHY_RX_TIME2_HS_RX_ANA0(4));

	/*
	 * Formula from the Allwinner BSP, with hardcoded coefficients
	 * (probably internal divider/multiplier).
	 */
	lprst_dly = 4 * (unsigned int)(dphy_clk_rate / (mipi_symbol_rate / 2));

	regmap_write(dphy->regs, SUN6I_DPHY_RX_TIME3_REG,
		     SUN6I_DPHY_RX_TIME3_LPRST_DLY(lprst_dly));

	/* Analog parameters are hardcoded in the Allwinner BSP. */
	regmap_write(dphy->regs, SUN6I_DPHY_ANA0_REG,
		     SUN6I_DPHY_ANA0_REG_PWS |
		     SUN6I_DPHY_ANA0_REG_SLV(7) |
		     SUN6I_DPHY_ANA0_REG_SFB(2));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA1_REG,
		     SUN6I_DPHY_ANA1_REG_SVTT(4));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA4_REG,
		     SUN6I_DPHY_ANA4_REG_DMPLVC |
		     SUN6I_DPHY_ANA4_REG_DMPLVD(1));

	regmap_write(dphy->regs, SUN6I_DPHY_ANA2_REG,
		     SUN6I_DPHY_ANA2_REG_ENIB);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA3_REG,
		     SUN6I_DPHY_ANA3_EN_LDOR |
		     SUN6I_DPHY_ANA3_EN_LDOC |
		     SUN6I_DPHY_ANA3_EN_LDOD);

	/*
	 * Delay comes from the Allwinner BSP, likely for internal regulator
	 * ramp-up.
	 */
	udelay(3);

	value = SUN6I_DPHY_RX_CTL_EN_DBC | SUN6I_DPHY_RX_CTL_RX_CLK_FORCE;

	/*
	 * Rx data lane force-enable bits are used as regular RX enable by the
	 * Allwinner BSP.
	 */
	if (dphy->config.lanes >= 1)
		value |= SUN6I_DPHY_RX_CTL_RX_D0_FORCE;
	if (dphy->config.lanes >= 2)
		value |= SUN6I_DPHY_RX_CTL_RX_D1_FORCE;
	if (dphy->config.lanes >= 3)
		value |= SUN6I_DPHY_RX_CTL_RX_D2_FORCE;
	if (dphy->config.lanes == 4)
		value |= SUN6I_DPHY_RX_CTL_RX_D3_FORCE;

	regmap_write(dphy->regs, SUN6I_DPHY_RX_CTL_REG, value);

	regmap_write(dphy->regs, SUN6I_DPHY_GCTL_REG,
		     SUN6I_DPHY_GCTL_LANE_NUM(dphy->config.lanes) |
		     SUN6I_DPHY_GCTL_EN);

	return 0;
}

static int sun6i_dphy_power_on(struct phy *phy)
{
	struct sun6i_dphy *dphy = phy_get_drvdata(phy);

	switch (dphy->direction) {
	case SUN6I_DPHY_DIRECTION_TX:
		return sun6i_dphy_tx_power_on(dphy);
	case SUN6I_DPHY_DIRECTION_RX:
		return sun6i_dphy_rx_power_on(dphy);
	default:
		return -EINVAL;
	}
}

static int sun6i_dphy_power_off(struct phy *phy)
{
	struct sun6i_dphy *dphy = phy_get_drvdata(phy);

	regmap_write(dphy->regs, SUN6I_DPHY_GCTL_REG, 0);

	regmap_write(dphy->regs, SUN6I_DPHY_ANA0_REG, 0);
	regmap_write(dphy->regs, SUN6I_DPHY_ANA1_REG, 0);
	regmap_write(dphy->regs, SUN6I_DPHY_ANA2_REG, 0);
	regmap_write(dphy->regs, SUN6I_DPHY_ANA3_REG, 0);
	regmap_write(dphy->regs, SUN6I_DPHY_ANA4_REG, 0);

	return 0;
}

static int sun6i_dphy_exit(struct phy *phy)
{
	struct sun6i_dphy *dphy = phy_get_drvdata(phy);

	clk_rate_exclusive_put(dphy->mod_clk);
	clk_disable_unprepare(dphy->mod_clk);
	reset_control_assert(dphy->reset);

	return 0;
}


static const struct phy_ops sun6i_dphy_ops = {
	.configure	= sun6i_dphy_configure,
	.power_on	= sun6i_dphy_power_on,
	.power_off	= sun6i_dphy_power_off,
	.init		= sun6i_dphy_init,
	.exit		= sun6i_dphy_exit,
};

static const struct regmap_config sun6i_dphy_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= SUN50I_COMBO_PHY_REG2,
	.name		= "mipi-dphy",
};

static int sun6i_dphy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct sun6i_dphy *dphy;
	const char *direction;
	void __iomem *regs;
	int ret;

	dphy = devm_kzalloc(&pdev->dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;

	dphy->variant = device_get_match_data(&pdev->dev);
	if (!dphy->variant)
		return -EINVAL;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Couldn't map the DPHY encoder registers\n");
		return PTR_ERR(regs);
	}

	dphy->regs = devm_regmap_init_mmio_clk(&pdev->dev, "bus",
					       regs, &sun6i_dphy_regmap_config);
	if (IS_ERR(dphy->regs)) {
		dev_err(&pdev->dev, "Couldn't create the DPHY encoder regmap\n");
		return PTR_ERR(dphy->regs);
	}

	dphy->reset = devm_reset_control_get_shared(&pdev->dev, NULL);
	if (IS_ERR(dphy->reset)) {
		dev_err(&pdev->dev, "Couldn't get our reset line\n");
		return PTR_ERR(dphy->reset);
	}

	dphy->mod_clk = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(dphy->mod_clk)) {
		dev_err(&pdev->dev, "Couldn't get the DPHY mod clock\n");
		return PTR_ERR(dphy->mod_clk);
	}

	dphy->phy = devm_phy_create(&pdev->dev, NULL, &sun6i_dphy_ops);
	if (IS_ERR(dphy->phy)) {
		dev_err(&pdev->dev, "failed to create PHY\n");
		return PTR_ERR(dphy->phy);
	}

	dphy->direction = SUN6I_DPHY_DIRECTION_TX;

	ret = of_property_read_string(pdev->dev.of_node, "allwinner,direction",
				      &direction);

	if (!ret && !strncmp(direction, "rx", 2)) {
		if (!dphy->variant->rx_supported) {
			dev_err(&pdev->dev, "RX not supported on this variant\n");
			return -EOPNOTSUPP;
		}

		dphy->direction = SUN6I_DPHY_DIRECTION_RX;
	}

	phy_set_drvdata(dphy->phy, dphy);
	phy_provider = devm_of_phy_provider_register(&pdev->dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct sun6i_dphy_variant sun6i_a31_mipi_dphy_variant = {
	.tx_power_on	= sun6i_a31_mipi_dphy_tx_power_on,
	.rx_supported	= true,
};

static const struct sun6i_dphy_variant sun50i_a100_mipi_dphy_variant = {
	.tx_power_on	= sun50i_a100_mipi_dphy_tx_power_on,
};

static const struct of_device_id sun6i_dphy_of_table[] = {
	{
		.compatible	= "allwinner,sun6i-a31-mipi-dphy",
		.data		= &sun6i_a31_mipi_dphy_variant,
	},
	{
		.compatible	= "allwinner,sun50i-a100-mipi-dphy",
		.data		= &sun50i_a100_mipi_dphy_variant,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sun6i_dphy_of_table);

static struct platform_driver sun6i_dphy_platform_driver = {
	.probe		= sun6i_dphy_probe,
	.driver		= {
		.name		= "sun6i-mipi-dphy",
		.of_match_table	= sun6i_dphy_of_table,
	},
};
module_platform_driver(sun6i_dphy_platform_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@bootlin>");
MODULE_DESCRIPTION("Allwinner A31 MIPI D-PHY Driver");
MODULE_LICENSE("GPL");
