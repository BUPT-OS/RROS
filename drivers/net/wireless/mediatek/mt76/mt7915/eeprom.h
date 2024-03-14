/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7915_EEPROM_H
#define __MT7915_EEPROM_H

#include "mt7915.h"

struct cal_data {
	u8 count;
	u16 offset[60];
};

enum mt7915_eeprom_field {
	MT_EE_CHIP_ID =		0x000,
	MT_EE_VERSION =		0x002,
	MT_EE_MAC_ADDR =	0x004,
	MT_EE_MAC_ADDR2 =	0x00a,
	MT_EE_DDIE_FT_VERSION =	0x050,
	MT_EE_DO_PRE_CAL =	0x062,
	MT_EE_WIFI_CONF =	0x190,
	MT_EE_RATE_DELTA_2G =	0x252,
	MT_EE_RATE_DELTA_5G =	0x29d,
	MT_EE_TX0_POWER_2G =	0x2fc,
	MT_EE_TX0_POWER_5G =	0x34b,
	MT_EE_RATE_DELTA_2G_V2 = 0x7d3,
	MT_EE_RATE_DELTA_5G_V2 = 0x81e,
	MT_EE_RATE_DELTA_6G_V2 = 0x884, /* 6g fields only appear in eeprom v2 */
	MT_EE_TX0_POWER_2G_V2 =	0x441,
	MT_EE_TX0_POWER_5G_V2 =	0x445,
	MT_EE_TX0_POWER_6G_V2 =	0x465,
	MT_EE_ADIE_FT_VERSION =	0x9a0,

	__MT_EE_MAX =		0xe00,
	__MT_EE_MAX_V2 =	0x1000,
	/* 0xe10 ~ 0x5780 used to save group cal data */
	MT_EE_PRECAL =		0xe10,
	MT_EE_PRECAL_V2 =	0x1010
};

#define MT_EE_WIFI_CAL_GROUP			BIT(0)
#define MT_EE_WIFI_CAL_DPD			GENMASK(2, 1)
#define MT_EE_CAL_UNIT				1024
#define MT_EE_CAL_GROUP_SIZE			(49 * MT_EE_CAL_UNIT + 16)
#define MT_EE_CAL_DPD_SIZE			(54 * MT_EE_CAL_UNIT)

#define MT_EE_WIFI_CONF0_TX_PATH		GENMASK(2, 0)
#define MT_EE_WIFI_CONF0_BAND_SEL		GENMASK(7, 6)
#define MT_EE_WIFI_CONF1_BAND_SEL		GENMASK(7, 6)
#define MT_EE_WIFI_CONF_STREAM_NUM		GENMASK(7, 5)
#define MT_EE_WIFI_CONF3_TX_PATH_B0		GENMASK(1, 0)
#define MT_EE_WIFI_CONF3_TX_PATH_B1		GENMASK(5, 4)
#define MT_EE_WIFI_CONF7_TSSI0_2G		BIT(0)
#define MT_EE_WIFI_CONF7_TSSI0_5G		BIT(2)
#define MT_EE_WIFI_CONF7_TSSI1_5G		BIT(4)

#define MT_EE_RATE_DELTA_MASK			GENMASK(5, 0)
#define MT_EE_RATE_DELTA_SIGN			BIT(6)
#define MT_EE_RATE_DELTA_EN			BIT(7)

enum mt7915_adie_sku {
	MT7976_ONE_ADIE_DBDC = 0x7,
	MT7975_ONE_ADIE	= 0x8,
	MT7976_ONE_ADIE	= 0xa,
	MT7975_DUAL_ADIE = 0xd,
	MT7976_DUAL_ADIE = 0xf,
};

enum mt7915_eeprom_band {
	MT_EE_BAND_SEL_DEFAULT,
	MT_EE_BAND_SEL_5GHZ,
	MT_EE_BAND_SEL_2GHZ,
	MT_EE_BAND_SEL_DUAL,
};

enum {
	MT_EE_V2_BAND_SEL_2GHZ,
	MT_EE_V2_BAND_SEL_5GHZ,
	MT_EE_V2_BAND_SEL_6GHZ,
	MT_EE_V2_BAND_SEL_5GHZ_6GHZ,
};

enum mt7915_sku_rate_group {
	SKU_CCK,
	SKU_OFDM,
	SKU_HT_BW20,
	SKU_HT_BW40,
	SKU_VHT_BW20,
	SKU_VHT_BW40,
	SKU_VHT_BW80,
	SKU_VHT_BW160,
	SKU_HE_RU26,
	SKU_HE_RU52,
	SKU_HE_RU106,
	SKU_HE_RU242,
	SKU_HE_RU484,
	SKU_HE_RU996,
	SKU_HE_RU2x996,
	MAX_SKU_RATE_GROUP_NUM,
};

static inline int
mt7915_get_channel_group_5g(int channel, bool is_7976)
{
	if (is_7976) {
		if (channel <= 64)
			return 0;
		if (channel <= 96)
			return 1;
		if (channel <= 128)
			return 2;
		if (channel <= 144)
			return 3;
		return 4;
	}

	if (channel >= 184 && channel <= 196)
		return 0;
	if (channel <= 48)
		return 1;
	if (channel <= 64)
		return 2;
	if (channel <= 96)
		return 3;
	if (channel <= 112)
		return 4;
	if (channel <= 128)
		return 5;
	if (channel <= 144)
		return 6;
	return 7;
}

static inline int
mt7915_get_channel_group_6g(int channel)
{
	if (channel <= 29)
		return 0;

	return DIV_ROUND_UP(channel - 29, 32);
}

static inline bool
mt7915_tssi_enabled(struct mt7915_dev *dev, enum nl80211_band band)
{
	u8 *eep = dev->mt76.eeprom.data;
	u8 val = eep[MT_EE_WIFI_CONF + 7];

	if (band == NL80211_BAND_2GHZ)
		return val & MT_EE_WIFI_CONF7_TSSI0_2G;

	if (dev->dbdc_support)
		return val & MT_EE_WIFI_CONF7_TSSI1_5G;
	else
		return val & MT_EE_WIFI_CONF7_TSSI0_5G;
}

extern const u8 mt7915_sku_group_len[MAX_SKU_RATE_GROUP_NUM];

#endif
