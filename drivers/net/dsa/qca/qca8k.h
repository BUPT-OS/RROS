/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#ifndef __QCA8K_H
#define __QCA8K_H

#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/dsa/tag_qca.h>

#define QCA8K_ETHERNET_MDIO_PRIORITY			7
#define QCA8K_ETHERNET_PHY_PRIORITY			6
#define QCA8K_ETHERNET_TIMEOUT				5

#define QCA8K_NUM_PORTS					7
#define QCA8K_NUM_CPU_PORTS				2
#define QCA8K_MAX_MTU					9000
#define QCA8K_NUM_LAGS					4
#define QCA8K_NUM_PORTS_FOR_LAG				4

#define PHY_ID_QCA8327					0x004dd034
#define QCA8K_ID_QCA8327				0x12
#define PHY_ID_QCA8337					0x004dd036
#define QCA8K_ID_QCA8337				0x13

#define QCA8K_QCA832X_MIB_COUNT				39
#define QCA8K_QCA833X_MIB_COUNT				41

#define QCA8K_BUSY_WAIT_TIMEOUT				2000

#define QCA8K_NUM_FDB_RECORDS				2048

#define QCA8K_PORT_VID_DEF				1

/* Global control registers */
#define QCA8K_REG_MASK_CTRL				0x000
#define   QCA8K_MASK_CTRL_REV_ID_MASK			GENMASK(7, 0)
#define   QCA8K_MASK_CTRL_REV_ID(x)			FIELD_GET(QCA8K_MASK_CTRL_REV_ID_MASK, x)
#define   QCA8K_MASK_CTRL_DEVICE_ID_MASK		GENMASK(15, 8)
#define   QCA8K_MASK_CTRL_DEVICE_ID(x)			FIELD_GET(QCA8K_MASK_CTRL_DEVICE_ID_MASK, x)
#define QCA8K_REG_PORT0_PAD_CTRL			0x004
#define   QCA8K_PORT0_PAD_MAC06_EXCHANGE_EN		BIT(31)
#define   QCA8K_PORT0_PAD_SGMII_RXCLK_FALLING_EDGE	BIT(19)
#define   QCA8K_PORT0_PAD_SGMII_TXCLK_FALLING_EDGE	BIT(18)
#define QCA8K_REG_PORT5_PAD_CTRL			0x008
#define QCA8K_REG_PORT6_PAD_CTRL			0x00c
#define   QCA8K_PORT_PAD_RGMII_EN			BIT(26)
#define   QCA8K_PORT_PAD_RGMII_TX_DELAY_MASK		GENMASK(23, 22)
#define   QCA8K_PORT_PAD_RGMII_TX_DELAY(x)		FIELD_PREP(QCA8K_PORT_PAD_RGMII_TX_DELAY_MASK, x)
#define   QCA8K_PORT_PAD_RGMII_RX_DELAY_MASK		GENMASK(21, 20)
#define   QCA8K_PORT_PAD_RGMII_RX_DELAY(x)		FIELD_PREP(QCA8K_PORT_PAD_RGMII_RX_DELAY_MASK, x)
#define	  QCA8K_PORT_PAD_RGMII_TX_DELAY_EN		BIT(25)
#define   QCA8K_PORT_PAD_RGMII_RX_DELAY_EN		BIT(24)
#define   QCA8K_PORT_PAD_SGMII_EN			BIT(7)
#define QCA8K_REG_PWS					0x010
#define   QCA8K_PWS_POWER_ON_SEL			BIT(31)
/* This reg is only valid for QCA832x and toggle the package
 * type from 176 pin (by default) to 148 pin used on QCA8327
 */
#define   QCA8327_PWS_PACKAGE148_EN			BIT(30)
#define   QCA8K_PWS_LED_OPEN_EN_CSR			BIT(24)
#define   QCA8K_PWS_SERDES_AEN_DIS			BIT(7)
#define QCA8K_REG_MODULE_EN				0x030
#define   QCA8K_MODULE_EN_MIB				BIT(0)
#define QCA8K_REG_MIB					0x034
#define   QCA8K_MIB_FUNC				GENMASK(26, 24)
#define   QCA8K_MIB_CPU_KEEP				BIT(20)
#define   QCA8K_MIB_BUSY				BIT(17)
#define QCA8K_MDIO_MASTER_CTRL				0x3c
#define   QCA8K_MDIO_MASTER_BUSY			BIT(31)
#define   QCA8K_MDIO_MASTER_EN				BIT(30)
#define   QCA8K_MDIO_MASTER_READ			BIT(27)
#define   QCA8K_MDIO_MASTER_WRITE			0
#define   QCA8K_MDIO_MASTER_SUP_PRE			BIT(26)
#define   QCA8K_MDIO_MASTER_PHY_ADDR_MASK		GENMASK(25, 21)
#define   QCA8K_MDIO_MASTER_PHY_ADDR(x)			FIELD_PREP(QCA8K_MDIO_MASTER_PHY_ADDR_MASK, x)
#define   QCA8K_MDIO_MASTER_REG_ADDR_MASK		GENMASK(20, 16)
#define   QCA8K_MDIO_MASTER_REG_ADDR(x)			FIELD_PREP(QCA8K_MDIO_MASTER_REG_ADDR_MASK, x)
#define   QCA8K_MDIO_MASTER_DATA_MASK			GENMASK(15, 0)
#define   QCA8K_MDIO_MASTER_DATA(x)			FIELD_PREP(QCA8K_MDIO_MASTER_DATA_MASK, x)
#define   QCA8K_MDIO_MASTER_MAX_PORTS			5
#define   QCA8K_MDIO_MASTER_MAX_REG			32

/* LED control register */
#define QCA8K_LED_PORT_COUNT				3
#define QCA8K_LED_COUNT					((QCA8K_NUM_PORTS - QCA8K_NUM_CPU_PORTS) * QCA8K_LED_PORT_COUNT)
#define QCA8K_LED_RULE_COUNT				6
#define QCA8K_LED_RULE_MAX				11
#define QCA8K_LED_PORT_INDEX(_phy, _led)		(((_phy) * QCA8K_LED_PORT_COUNT) + (_led))

#define QCA8K_LED_PHY123_PATTERN_EN_SHIFT(_phy, _led)	((((_phy) - 1) * 6) + 8 + (2 * (_led)))
#define QCA8K_LED_PHY123_PATTERN_EN_MASK		GENMASK(1, 0)

#define QCA8K_LED_PHY0123_CONTROL_RULE_SHIFT		0
#define QCA8K_LED_PHY4_CONTROL_RULE_SHIFT		16

#define QCA8K_LED_CTRL_REG(_i)				(0x050 + (_i) * 4)
#define QCA8K_LED_CTRL0_REG				0x50
#define QCA8K_LED_CTRL1_REG				0x54
#define QCA8K_LED_CTRL2_REG				0x58
#define QCA8K_LED_CTRL3_REG				0x5C
#define   QCA8K_LED_CTRL_SHIFT(_i)			(((_i) % 2) * 16)
#define   QCA8K_LED_CTRL_MASK				GENMASK(15, 0)
#define QCA8K_LED_RULE_MASK				GENMASK(13, 0)
#define QCA8K_LED_BLINK_FREQ_MASK			GENMASK(1, 0)
#define QCA8K_LED_BLINK_FREQ_SHITF			0
#define   QCA8K_LED_BLINK_2HZ				0
#define   QCA8K_LED_BLINK_4HZ				1
#define   QCA8K_LED_BLINK_8HZ				2
#define   QCA8K_LED_BLINK_AUTO				3
#define QCA8K_LED_LINKUP_OVER_MASK			BIT(2)
#define QCA8K_LED_TX_BLINK_MASK				BIT(4)
#define QCA8K_LED_RX_BLINK_MASK				BIT(5)
#define QCA8K_LED_COL_BLINK_MASK			BIT(7)
#define QCA8K_LED_LINK_10M_EN_MASK			BIT(8)
#define QCA8K_LED_LINK_100M_EN_MASK			BIT(9)
#define QCA8K_LED_LINK_1000M_EN_MASK			BIT(10)
#define QCA8K_LED_POWER_ON_LIGHT_MASK			BIT(11)
#define QCA8K_LED_HALF_DUPLEX_MASK			BIT(12)
#define QCA8K_LED_FULL_DUPLEX_MASK			BIT(13)
#define QCA8K_LED_PATTERN_EN_MASK			GENMASK(15, 14)
#define QCA8K_LED_PATTERN_EN_SHIFT			14
#define   QCA8K_LED_ALWAYS_OFF				0
#define   QCA8K_LED_ALWAYS_BLINK_4HZ			1
#define   QCA8K_LED_ALWAYS_ON				2
#define   QCA8K_LED_RULE_CONTROLLED			3

#define QCA8K_GOL_MAC_ADDR0				0x60
#define QCA8K_GOL_MAC_ADDR1				0x64
#define QCA8K_MAX_FRAME_SIZE				0x78
#define QCA8K_REG_PORT_STATUS(_i)			(0x07c + (_i) * 4)
#define   QCA8K_PORT_STATUS_SPEED			GENMASK(1, 0)
#define   QCA8K_PORT_STATUS_SPEED_10			0
#define   QCA8K_PORT_STATUS_SPEED_100			0x1
#define   QCA8K_PORT_STATUS_SPEED_1000			0x2
#define   QCA8K_PORT_STATUS_TXMAC			BIT(2)
#define   QCA8K_PORT_STATUS_RXMAC			BIT(3)
#define   QCA8K_PORT_STATUS_TXFLOW			BIT(4)
#define   QCA8K_PORT_STATUS_RXFLOW			BIT(5)
#define   QCA8K_PORT_STATUS_DUPLEX			BIT(6)
#define   QCA8K_PORT_STATUS_LINK_UP			BIT(8)
#define   QCA8K_PORT_STATUS_LINK_AUTO			BIT(9)
#define   QCA8K_PORT_STATUS_LINK_PAUSE			BIT(10)
#define   QCA8K_PORT_STATUS_FLOW_AUTO			BIT(12)
#define QCA8K_REG_PORT_HDR_CTRL(_i)			(0x9c + (_i * 4))
#define   QCA8K_PORT_HDR_CTRL_RX_MASK			GENMASK(3, 2)
#define   QCA8K_PORT_HDR_CTRL_TX_MASK			GENMASK(1, 0)
#define   QCA8K_PORT_HDR_CTRL_ALL			2
#define   QCA8K_PORT_HDR_CTRL_MGMT			1
#define   QCA8K_PORT_HDR_CTRL_NONE			0
#define QCA8K_REG_SGMII_CTRL				0x0e0
#define   QCA8K_SGMII_EN_PLL				BIT(1)
#define   QCA8K_SGMII_EN_RX				BIT(2)
#define   QCA8K_SGMII_EN_TX				BIT(3)
#define   QCA8K_SGMII_EN_SD				BIT(4)
#define   QCA8K_SGMII_CLK125M_DELAY			BIT(7)
#define   QCA8K_SGMII_MODE_CTRL_MASK			GENMASK(23, 22)
#define   QCA8K_SGMII_MODE_CTRL(x)			FIELD_PREP(QCA8K_SGMII_MODE_CTRL_MASK, x)
#define   QCA8K_SGMII_MODE_CTRL_BASEX			QCA8K_SGMII_MODE_CTRL(0x0)
#define   QCA8K_SGMII_MODE_CTRL_PHY			QCA8K_SGMII_MODE_CTRL(0x1)
#define   QCA8K_SGMII_MODE_CTRL_MAC			QCA8K_SGMII_MODE_CTRL(0x2)

/* MAC_PWR_SEL registers */
#define QCA8K_REG_MAC_PWR_SEL				0x0e4
#define   QCA8K_MAC_PWR_RGMII1_1_8V			BIT(18)
#define   QCA8K_MAC_PWR_RGMII0_1_8V			BIT(19)

/* EEE control registers */
#define QCA8K_REG_EEE_CTRL				0x100
#define  QCA8K_REG_EEE_CTRL_LPI_EN(_i)			((_i + 1) * 2)

/* TRUNK_HASH_EN registers */
#define QCA8K_TRUNK_HASH_EN_CTRL			0x270
#define   QCA8K_TRUNK_HASH_SIP_EN			BIT(3)
#define   QCA8K_TRUNK_HASH_DIP_EN			BIT(2)
#define   QCA8K_TRUNK_HASH_SA_EN			BIT(1)
#define   QCA8K_TRUNK_HASH_DA_EN			BIT(0)
#define   QCA8K_TRUNK_HASH_MASK				GENMASK(3, 0)

/* ACL registers */
#define QCA8K_REG_PORT_VLAN_CTRL0(_i)			(0x420 + (_i * 8))
#define   QCA8K_PORT_VLAN_CVID_MASK			GENMASK(27, 16)
#define   QCA8K_PORT_VLAN_CVID(x)			FIELD_PREP(QCA8K_PORT_VLAN_CVID_MASK, x)
#define   QCA8K_PORT_VLAN_SVID_MASK			GENMASK(11, 0)
#define   QCA8K_PORT_VLAN_SVID(x)			FIELD_PREP(QCA8K_PORT_VLAN_SVID_MASK, x)
#define QCA8K_REG_PORT_VLAN_CTRL1(_i)			(0x424 + (_i * 8))
#define QCA8K_REG_IPV4_PRI_BASE_ADDR			0x470
#define QCA8K_REG_IPV4_PRI_ADDR_MASK			0x474

/* Lookup registers */
#define QCA8K_ATU_TABLE_SIZE				3 /* 12 bytes wide table / sizeof(u32) */

#define QCA8K_REG_ATU_DATA0				0x600
#define   QCA8K_ATU_ADDR2_MASK				GENMASK(31, 24)
#define   QCA8K_ATU_ADDR3_MASK				GENMASK(23, 16)
#define   QCA8K_ATU_ADDR4_MASK				GENMASK(15, 8)
#define   QCA8K_ATU_ADDR5_MASK				GENMASK(7, 0)
#define QCA8K_REG_ATU_DATA1				0x604
#define   QCA8K_ATU_PORT_MASK				GENMASK(22, 16)
#define   QCA8K_ATU_ADDR0_MASK				GENMASK(15, 8)
#define   QCA8K_ATU_ADDR1_MASK				GENMASK(7, 0)
#define QCA8K_REG_ATU_DATA2				0x608
#define   QCA8K_ATU_VID_MASK				GENMASK(19, 8)
#define   QCA8K_ATU_STATUS_MASK				GENMASK(3, 0)
#define   QCA8K_ATU_STATUS_STATIC			0xf
#define QCA8K_REG_ATU_FUNC				0x60c
#define   QCA8K_ATU_FUNC_BUSY				BIT(31)
#define   QCA8K_ATU_FUNC_PORT_EN			BIT(14)
#define   QCA8K_ATU_FUNC_MULTI_EN			BIT(13)
#define   QCA8K_ATU_FUNC_FULL				BIT(12)
#define   QCA8K_ATU_FUNC_PORT_MASK			GENMASK(11, 8)
#define QCA8K_REG_VTU_FUNC0				0x610
#define   QCA8K_VTU_FUNC0_VALID				BIT(20)
#define   QCA8K_VTU_FUNC0_IVL_EN			BIT(19)
/*        QCA8K_VTU_FUNC0_EG_MODE_MASK			GENMASK(17, 4)
 *          It does contain VLAN_MODE for each port [5:4] for port0,
 *          [7:6] for port1 ... [17:16] for port6. Use virtual port
 *          define to handle this.
 */
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i)	(4 + (_i) * 2)
#define   QCA8K_VTU_FUNC0_EG_MODE_MASK			GENMASK(1, 0)
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_MASK(_i)		(GENMASK(1, 0) << QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i))
#define   QCA8K_VTU_FUNC0_EG_MODE_UNMOD			FIELD_PREP(QCA8K_VTU_FUNC0_EG_MODE_MASK, 0x0)
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_UNMOD(_i)	(QCA8K_VTU_FUNC0_EG_MODE_UNMOD << QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i))
#define   QCA8K_VTU_FUNC0_EG_MODE_UNTAG			FIELD_PREP(QCA8K_VTU_FUNC0_EG_MODE_MASK, 0x1)
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_UNTAG(_i)	(QCA8K_VTU_FUNC0_EG_MODE_UNTAG << QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i))
#define   QCA8K_VTU_FUNC0_EG_MODE_TAG			FIELD_PREP(QCA8K_VTU_FUNC0_EG_MODE_MASK, 0x2)
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_TAG(_i)		(QCA8K_VTU_FUNC0_EG_MODE_TAG << QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i))
#define   QCA8K_VTU_FUNC0_EG_MODE_NOT			FIELD_PREP(QCA8K_VTU_FUNC0_EG_MODE_MASK, 0x3)
#define   QCA8K_VTU_FUNC0_EG_MODE_PORT_NOT(_i)		(QCA8K_VTU_FUNC0_EG_MODE_NOT << QCA8K_VTU_FUNC0_EG_MODE_PORT_SHIFT(_i))
#define QCA8K_REG_VTU_FUNC1				0x614
#define   QCA8K_VTU_FUNC1_BUSY				BIT(31)
#define   QCA8K_VTU_FUNC1_VID_MASK			GENMASK(27, 16)
#define   QCA8K_VTU_FUNC1_FULL				BIT(4)
#define QCA8K_REG_ATU_CTRL				0x618
#define   QCA8K_ATU_AGE_TIME_MASK			GENMASK(15, 0)
#define   QCA8K_ATU_AGE_TIME(x)				FIELD_PREP(QCA8K_ATU_AGE_TIME_MASK, (x))
#define QCA8K_REG_GLOBAL_FW_CTRL0			0x620
#define   QCA8K_GLOBAL_FW_CTRL0_CPU_PORT_EN		BIT(10)
#define   QCA8K_GLOBAL_FW_CTRL0_MIRROR_PORT_NUM		GENMASK(7, 4)
#define QCA8K_REG_GLOBAL_FW_CTRL1			0x624
#define   QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_MASK		GENMASK(30, 24)
#define   QCA8K_GLOBAL_FW_CTRL1_BC_DP_MASK		GENMASK(22, 16)
#define   QCA8K_GLOBAL_FW_CTRL1_MC_DP_MASK		GENMASK(14, 8)
#define   QCA8K_GLOBAL_FW_CTRL1_UC_DP_MASK		GENMASK(6, 0)
#define QCA8K_PORT_LOOKUP_CTRL(_i)			(0x660 + (_i) * 0xc)
#define   QCA8K_PORT_LOOKUP_MEMBER			GENMASK(6, 0)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE_MASK		GENMASK(9, 8)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE(x)		FIELD_PREP(QCA8K_PORT_LOOKUP_VLAN_MODE_MASK, x)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE_NONE		QCA8K_PORT_LOOKUP_VLAN_MODE(0x0)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE_FALLBACK		QCA8K_PORT_LOOKUP_VLAN_MODE(0x1)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE_CHECK		QCA8K_PORT_LOOKUP_VLAN_MODE(0x2)
#define   QCA8K_PORT_LOOKUP_VLAN_MODE_SECURE		QCA8K_PORT_LOOKUP_VLAN_MODE(0x3)
#define   QCA8K_PORT_LOOKUP_STATE_MASK			GENMASK(18, 16)
#define   QCA8K_PORT_LOOKUP_STATE(x)			FIELD_PREP(QCA8K_PORT_LOOKUP_STATE_MASK, x)
#define   QCA8K_PORT_LOOKUP_STATE_DISABLED		QCA8K_PORT_LOOKUP_STATE(0x0)
#define   QCA8K_PORT_LOOKUP_STATE_BLOCKING		QCA8K_PORT_LOOKUP_STATE(0x1)
#define   QCA8K_PORT_LOOKUP_STATE_LISTENING		QCA8K_PORT_LOOKUP_STATE(0x2)
#define   QCA8K_PORT_LOOKUP_STATE_LEARNING		QCA8K_PORT_LOOKUP_STATE(0x3)
#define   QCA8K_PORT_LOOKUP_STATE_FORWARD		QCA8K_PORT_LOOKUP_STATE(0x4)
#define   QCA8K_PORT_LOOKUP_LEARN			BIT(20)
#define   QCA8K_PORT_LOOKUP_ING_MIRROR_EN		BIT(25)

#define QCA8K_REG_GOL_TRUNK_CTRL0			0x700
/* 4 max trunk first
 * first 6 bit for member bitmap
 * 7th bit is to enable trunk port
 */
#define QCA8K_REG_GOL_TRUNK_SHIFT(_i)			((_i) * 8)
#define QCA8K_REG_GOL_TRUNK_EN_MASK			BIT(7)
#define QCA8K_REG_GOL_TRUNK_EN(_i)			(QCA8K_REG_GOL_TRUNK_EN_MASK << QCA8K_REG_GOL_TRUNK_SHIFT(_i))
#define QCA8K_REG_GOL_TRUNK_MEMBER_MASK			GENMASK(6, 0)
#define QCA8K_REG_GOL_TRUNK_MEMBER(_i)			(QCA8K_REG_GOL_TRUNK_MEMBER_MASK << QCA8K_REG_GOL_TRUNK_SHIFT(_i))
/* 0x704 for TRUNK 0-1 --- 0x708 for TRUNK 2-3 */
#define QCA8K_REG_GOL_TRUNK_CTRL(_i)			(0x704 + (((_i) / 2) * 4))
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_MASK		GENMASK(3, 0)
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_EN_MASK		BIT(3)
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_PORT_MASK		GENMASK(2, 0)
#define QCA8K_REG_GOL_TRUNK_ID_SHIFT(_i)		(((_i) / 2) * 16)
#define QCA8K_REG_GOL_MEM_ID_SHIFT(_i)			((_i) * 4)
/* Complex shift: FIRST shift for port THEN shift for trunk */
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_SHIFT(_i, _j)	(QCA8K_REG_GOL_MEM_ID_SHIFT(_j) + QCA8K_REG_GOL_TRUNK_ID_SHIFT(_i))
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_EN(_i, _j)	(QCA8K_REG_GOL_TRUNK_ID_MEM_ID_EN_MASK << QCA8K_REG_GOL_TRUNK_ID_MEM_ID_SHIFT(_i, _j))
#define QCA8K_REG_GOL_TRUNK_ID_MEM_ID_PORT(_i, _j)	(QCA8K_REG_GOL_TRUNK_ID_MEM_ID_PORT_MASK << QCA8K_REG_GOL_TRUNK_ID_MEM_ID_SHIFT(_i, _j))

#define QCA8K_REG_GLOBAL_FC_THRESH			0x800
#define   QCA8K_GLOBAL_FC_GOL_XON_THRES_MASK		GENMASK(24, 16)
#define   QCA8K_GLOBAL_FC_GOL_XON_THRES(x)		FIELD_PREP(QCA8K_GLOBAL_FC_GOL_XON_THRES_MASK, x)
#define   QCA8K_GLOBAL_FC_GOL_XOFF_THRES_MASK		GENMASK(8, 0)
#define   QCA8K_GLOBAL_FC_GOL_XOFF_THRES(x)		FIELD_PREP(QCA8K_GLOBAL_FC_GOL_XOFF_THRES_MASK, x)

#define QCA8K_REG_PORT_HOL_CTRL0(_i)			(0x970 + (_i) * 0x8)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI0_BUF_MASK		GENMASK(3, 0)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI0(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI0_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI1_BUF_MASK		GENMASK(7, 4)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI1(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI1_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI2_BUF_MASK		GENMASK(11, 8)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI2(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI2_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI3_BUF_MASK		GENMASK(15, 12)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI3(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI3_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI4_BUF_MASK		GENMASK(19, 16)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI4(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI4_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI5_BUF_MASK		GENMASK(23, 20)
#define   QCA8K_PORT_HOL_CTRL0_EG_PRI5(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PRI5_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL0_EG_PORT_BUF_MASK		GENMASK(29, 24)
#define   QCA8K_PORT_HOL_CTRL0_EG_PORT(x)		FIELD_PREP(QCA8K_PORT_HOL_CTRL0_EG_PORT_BUF_MASK, x)

#define QCA8K_REG_PORT_HOL_CTRL1(_i)			(0x974 + (_i) * 0x8)
#define   QCA8K_PORT_HOL_CTRL1_ING_BUF_MASK		GENMASK(3, 0)
#define   QCA8K_PORT_HOL_CTRL1_ING(x)			FIELD_PREP(QCA8K_PORT_HOL_CTRL1_ING_BUF_MASK, x)
#define   QCA8K_PORT_HOL_CTRL1_EG_PRI_BUF_EN		BIT(6)
#define   QCA8K_PORT_HOL_CTRL1_EG_PORT_BUF_EN		BIT(7)
#define   QCA8K_PORT_HOL_CTRL1_WRED_EN			BIT(8)
#define   QCA8K_PORT_HOL_CTRL1_EG_MIRROR_EN		BIT(16)

/* Pkt edit registers */
#define QCA8K_EGREES_VLAN_PORT_SHIFT(_i)		(16 * ((_i) % 2))
#define QCA8K_EGREES_VLAN_PORT_MASK(_i)			(GENMASK(11, 0) << QCA8K_EGREES_VLAN_PORT_SHIFT(_i))
#define QCA8K_EGREES_VLAN_PORT(_i, x)			((x) << QCA8K_EGREES_VLAN_PORT_SHIFT(_i))
#define QCA8K_EGRESS_VLAN(x)				(0x0c70 + (4 * (x / 2)))

/* L3 registers */
#define QCA8K_HROUTER_CONTROL				0xe00
#define   QCA8K_HROUTER_CONTROL_GLB_LOCKTIME_M		GENMASK(17, 16)
#define   QCA8K_HROUTER_CONTROL_GLB_LOCKTIME_S		16
#define   QCA8K_HROUTER_CONTROL_ARP_AGE_MODE		1
#define QCA8K_HROUTER_PBASED_CONTROL1			0xe08
#define QCA8K_HROUTER_PBASED_CONTROL2			0xe0c
#define QCA8K_HNAT_CONTROL				0xe38

/* MIB registers */
#define QCA8K_PORT_MIB_COUNTER(_i)			(0x1000 + (_i) * 0x100)

/* QCA specific MII registers */
#define MII_ATH_MMD_ADDR				0x0d
#define MII_ATH_MMD_DATA				0x0e

enum {
	QCA8K_PORT_SPEED_10M = 0,
	QCA8K_PORT_SPEED_100M = 1,
	QCA8K_PORT_SPEED_1000M = 2,
	QCA8K_PORT_SPEED_ERR = 3,
};

enum qca8k_fdb_cmd {
	QCA8K_FDB_FLUSH	= 1,
	QCA8K_FDB_LOAD = 2,
	QCA8K_FDB_PURGE = 3,
	QCA8K_FDB_FLUSH_PORT = 5,
	QCA8K_FDB_NEXT = 6,
	QCA8K_FDB_SEARCH = 7,
};

enum qca8k_vlan_cmd {
	QCA8K_VLAN_FLUSH = 1,
	QCA8K_VLAN_LOAD = 2,
	QCA8K_VLAN_PURGE = 3,
	QCA8K_VLAN_REMOVE_PORT = 4,
	QCA8K_VLAN_NEXT = 5,
	QCA8K_VLAN_READ = 6,
};

enum qca8k_mid_cmd {
	QCA8K_MIB_FLUSH = 1,
	QCA8K_MIB_FLUSH_PORT = 2,
	QCA8K_MIB_CAST = 3,
};

struct qca8k_priv;

struct qca8k_info_ops {
	int (*autocast_mib)(struct dsa_switch *ds, int port, u64 *data);
};

struct qca8k_match_data {
	u8 id;
	bool reduced_package;
	u8 mib_count;
	const struct qca8k_info_ops *ops;
};

enum {
	QCA8K_CPU_PORT0,
	QCA8K_CPU_PORT6,
};

struct qca8k_mgmt_eth_data {
	struct completion rw_done;
	struct mutex mutex; /* Enforce one mdio read/write at time */
	bool ack;
	u32 seq;
	u32 data[4];
};

struct qca8k_mib_eth_data {
	struct completion rw_done;
	struct mutex mutex; /* Process one command at time */
	refcount_t port_parsed; /* Counter to track parsed port */
	u8 req_port;
	u64 *data; /* pointer to ethtool data */
};

struct qca8k_ports_config {
	bool sgmii_rx_clk_falling_edge;
	bool sgmii_tx_clk_falling_edge;
	bool sgmii_enable_pll;
	u8 rgmii_rx_delay[QCA8K_NUM_CPU_PORTS]; /* 0: CPU port0, 1: CPU port6 */
	u8 rgmii_tx_delay[QCA8K_NUM_CPU_PORTS]; /* 0: CPU port0, 1: CPU port6 */
};

struct qca8k_mdio_cache {
/* The 32bit switch registers are accessed indirectly. To achieve this we need
 * to set the page of the register. Track the last page that was set to reduce
 * mdio writes
 */
	u16 page;
};

struct qca8k_pcs {
	struct phylink_pcs pcs;
	struct qca8k_priv *priv;
	int port;
};

struct qca8k_led_pattern_en {
	u32 reg;
	u8 shift;
};

struct qca8k_led {
	u8 port_num;
	u8 led_num;
	u16 old_rule;
	struct qca8k_priv *priv;
	struct led_classdev cdev;
};

struct qca8k_priv {
	u8 switch_id;
	u8 switch_revision;
	u8 mirror_rx;
	u8 mirror_tx;
	u8 lag_hash_mode;
	/* Each bit correspond to a port. This switch can support a max of 7 port.
	 * Bit 1: port enabled. Bit 0: port disabled.
	 */
	u8 port_enabled_map;
	struct qca8k_ports_config ports_config;
	struct regmap *regmap;
	struct mii_bus *bus;
	struct dsa_switch *ds;
	struct mutex reg_mutex;
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct net_device *mgmt_master; /* Track if mdio/mib Ethernet is available */
	struct qca8k_mgmt_eth_data mgmt_eth_data;
	struct qca8k_mib_eth_data mib_eth_data;
	struct qca8k_mdio_cache mdio_cache;
	struct qca8k_pcs pcs_port_0;
	struct qca8k_pcs pcs_port_6;
	const struct qca8k_match_data *info;
	struct qca8k_led ports_led[QCA8K_LED_COUNT];
};

struct qca8k_mib_desc {
	unsigned int size;
	unsigned int offset;
	const char *name;
};

struct qca8k_fdb {
	u16 vid;
	u8 port_mask;
	u8 aging;
	u8 mac[6];
};

static inline u32 qca8k_port_to_phy(int port)
{
	/* From Andrew Lunn:
	 * Port 0 has no internal phy.
	 * Port 1 has an internal PHY at MDIO address 0.
	 * Port 2 has an internal PHY at MDIO address 1.
	 * ...
	 * Port 5 has an internal PHY at MDIO address 4.
	 * Port 6 has no internal PHY.
	 */

	return port - 1;
}

/* Common setup function */
extern const struct qca8k_mib_desc ar8327_mib[];
extern const struct regmap_access_table qca8k_readable_table;
int qca8k_mib_init(struct qca8k_priv *priv);
void qca8k_port_set_status(struct qca8k_priv *priv, int port, int enable);
int qca8k_read_switch_id(struct qca8k_priv *priv);

/* Common read/write/rmw function */
int qca8k_read(struct qca8k_priv *priv, u32 reg, u32 *val);
int qca8k_write(struct qca8k_priv *priv, u32 reg, u32 val);
int qca8k_rmw(struct qca8k_priv *priv, u32 reg, u32 mask, u32 write_val);

/* Common ops function */
void qca8k_fdb_flush(struct qca8k_priv *priv);

/* Common ethtool stats function */
void qca8k_get_strings(struct dsa_switch *ds, int port, u32 stringset, uint8_t *data);
void qca8k_get_ethtool_stats(struct dsa_switch *ds, int port,
			     uint64_t *data);
int qca8k_get_sset_count(struct dsa_switch *ds, int port, int sset);

/* Common eee function */
int qca8k_set_mac_eee(struct dsa_switch *ds, int port, struct ethtool_eee *eee);
int qca8k_get_mac_eee(struct dsa_switch *ds, int port, struct ethtool_eee *e);

/* Common bridge function */
void qca8k_port_stp_state_set(struct dsa_switch *ds, int port, u8 state);
int qca8k_port_pre_bridge_flags(struct dsa_switch *ds, int port,
				struct switchdev_brport_flags flags,
				struct netlink_ext_ack *extack);
int qca8k_port_bridge_flags(struct dsa_switch *ds, int port,
			    struct switchdev_brport_flags flags,
			    struct netlink_ext_ack *extack);
int qca8k_port_bridge_join(struct dsa_switch *ds, int port,
			   struct dsa_bridge bridge,
			   bool *tx_fwd_offload,
			   struct netlink_ext_ack *extack);
void qca8k_port_bridge_leave(struct dsa_switch *ds, int port,
			     struct dsa_bridge bridge);

/* Common port enable/disable function */
int qca8k_port_enable(struct dsa_switch *ds, int port,
		      struct phy_device *phy);
void qca8k_port_disable(struct dsa_switch *ds, int port);

/* Common MTU function */
int qca8k_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu);
int qca8k_port_max_mtu(struct dsa_switch *ds, int port);

/* Common fast age function */
void qca8k_port_fast_age(struct dsa_switch *ds, int port);
int qca8k_set_ageing_time(struct dsa_switch *ds, unsigned int msecs);

/* Common FDB function */
int qca8k_port_fdb_insert(struct qca8k_priv *priv, const u8 *addr,
			  u16 port_mask, u16 vid);
int qca8k_port_fdb_add(struct dsa_switch *ds, int port,
		       const unsigned char *addr, u16 vid,
		       struct dsa_db db);
int qca8k_port_fdb_del(struct dsa_switch *ds, int port,
		       const unsigned char *addr, u16 vid,
		       struct dsa_db db);
int qca8k_port_fdb_dump(struct dsa_switch *ds, int port,
			dsa_fdb_dump_cb_t *cb, void *data);

/* Common MDB function */
int qca8k_port_mdb_add(struct dsa_switch *ds, int port,
		       const struct switchdev_obj_port_mdb *mdb,
		       struct dsa_db db);
int qca8k_port_mdb_del(struct dsa_switch *ds, int port,
		       const struct switchdev_obj_port_mdb *mdb,
		       struct dsa_db db);

/* Common port mirror function */
int qca8k_port_mirror_add(struct dsa_switch *ds, int port,
			  struct dsa_mall_mirror_tc_entry *mirror,
			  bool ingress, struct netlink_ext_ack *extack);
void qca8k_port_mirror_del(struct dsa_switch *ds, int port,
			   struct dsa_mall_mirror_tc_entry *mirror);

/* Common port VLAN function */
int qca8k_port_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering,
			      struct netlink_ext_ack *extack);
int qca8k_port_vlan_add(struct dsa_switch *ds, int port,
			const struct switchdev_obj_port_vlan *vlan,
			struct netlink_ext_ack *extack);
int qca8k_port_vlan_del(struct dsa_switch *ds, int port,
			const struct switchdev_obj_port_vlan *vlan);

/* Common port LAG function */
int qca8k_port_lag_join(struct dsa_switch *ds, int port, struct dsa_lag lag,
			struct netdev_lag_upper_info *info,
			struct netlink_ext_ack *extack);
int qca8k_port_lag_leave(struct dsa_switch *ds, int port,
			 struct dsa_lag lag);

#endif /* __QCA8K_H */
