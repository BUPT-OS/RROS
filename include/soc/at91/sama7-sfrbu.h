/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Microchip SAMA7 SFRBU registers offsets and bit definitions.
 *
 * Copyright (C) [2020] Microchip Technology Inc. and its subsidiaries
 *
 * Author: Claudu Beznea <claudiu.beznea@microchip.com>
 */

#ifndef __SAMA7_SFRBU_H__
#define __SAMA7_SFRBU_H__

#ifdef CONFIG_SOC_SAMA7

#define AT91_SFRBU_PSWBU			(0x00)		/* SFRBU Power Switch BU Control Register */
#define		AT91_SFRBU_PSWBU_PSWKEY		(0x4BD20C << 8)	/* Specific value mandatory to allow writing of other register bits */
#define		AT91_SFRBU_PSWBU_STATE		(1 << 2)	/* Power switch BU state */
#define		AT91_SFRBU_PSWBU_SOFTSWITCH	(1 << 1)	/* Power switch BU source selection */
#define		AT91_SFRBU_PSWBU_CTRL		(1 << 0)	/* Power switch BU control */

#define AT91_SFRBU_25LDOCR			(0x0C)		/* SFRBU 2.5V LDO Control Register */
#define		AT91_SFRBU_25LDOCR_LDOANAKEY	(0x3B6E18 << 8)	/* Specific value mandatory to allow writing of other register bits. */
#define		AT91_SFRBU_25LDOCR_STATE	(1 << 3)	/* LDOANA Switch On/Off Control */
#define		AT91_SFRBU_25LDOCR_LP		(1 << 2)	/* LDOANA Low-Power Mode Control */
#define		AT91_SFRBU_PD_VALUE_MSK		(0x3)
#define		AT91_SFRBU_25LDOCR_PD_VALUE(v)	((v) & AT91_SFRBU_PD_VALUE_MSK)	/* LDOANA Pull-down value */

#define AT91_FRBU_DDRPWR			(0x10)		/* SFRBU DDR Power Control Register */
#define		AT91_FRBU_DDRPWR_STATE		(1 << 0)	/* DDR Power Mode State */

#endif /* CONFIG_SOC_SAMA7 */

#endif /* __SAMA7_SFRBU_H__ */

