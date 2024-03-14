/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2020, Intel Corporation
 * DWMAC Intel header file
 */

#ifndef __DWMAC_INTEL_H__
#define __DWMAC_INTEL_H__

#define POLL_DELAY_US 8

/* SERDES Register */
#define SERDES_GCR	0x0	/* Global Conguration */
#define SERDES_GSR0	0x5	/* Global Status Reg0 */
#define SERDES_GCR0	0xb	/* Global Configuration Reg0 */

/* SERDES defines */
#define SERDES_PLL_CLK		BIT(0)		/* PLL clk valid signal */
#define SERDES_PHY_RX_CLK	BIT(1)		/* PSE SGMII PHY rx clk */
#define SERDES_RST		BIT(2)		/* Serdes Reset */
#define SERDES_PWR_ST_MASK	GENMASK(6, 4)	/* Serdes Power state*/
#define SERDES_RATE_MASK	GENMASK(9, 8)
#define SERDES_PCLK_MASK	GENMASK(14, 12)	/* PCLK rate to PHY */
#define SERDES_LINK_MODE_MASK	GENMASK(2, 1)
#define SERDES_LINK_MODE_SHIFT	1
#define SERDES_PWR_ST_SHIFT	4
#define SERDES_PWR_ST_P0	0x0
#define SERDES_PWR_ST_P3	0x3
#define SERDES_LINK_MODE_2G5	0x3
#define SERSED_LINK_MODE_1G	0x2
#define SERDES_PCLK_37p5MHZ	0x0
#define SERDES_PCLK_70MHZ	0x1
#define SERDES_RATE_PCIE_GEN1	0x0
#define SERDES_RATE_PCIE_GEN2	0x1
#define SERDES_RATE_PCIE_SHIFT	8
#define SERDES_PCLK_SHIFT	12

#define INTEL_MGBE_ADHOC_ADDR	0x15
#define INTEL_MGBE_XPCS_ADDR	0x16

/* Cross-timestamping defines */
#define ART_CPUID_LEAF		0x15
#define EHL_PSE_ART_MHZ		19200000

/* Selection for PTP Clock Freq belongs to PSE & PCH GbE */
#define PSE_PTP_CLK_FREQ_MASK		(GMAC_GPO0 | GMAC_GPO3)
#define PSE_PTP_CLK_FREQ_19_2MHZ	(GMAC_GPO0)
#define PSE_PTP_CLK_FREQ_200MHZ		(GMAC_GPO0 | GMAC_GPO3)
#define PSE_PTP_CLK_FREQ_256MHZ		(0)
#define PCH_PTP_CLK_FREQ_MASK		(GMAC_GPO0)
#define PCH_PTP_CLK_FREQ_19_2MHZ	(GMAC_GPO0)
#define PCH_PTP_CLK_FREQ_200MHZ		(0)

#endif /* __DWMAC_INTEL_H__ */
