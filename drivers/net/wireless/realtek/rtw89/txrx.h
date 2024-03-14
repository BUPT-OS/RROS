/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2020  Realtek Corporation
 */

#ifndef __RTW89_TXRX_H__
#define __RTW89_TXRX_H__

#include "debug.h"

#define DATA_RATE_MODE_CTRL_MASK	GENMASK(8, 7)
#define DATA_RATE_MODE_CTRL_MASK_V1	GENMASK(10, 8)
#define DATA_RATE_NOT_HT_IDX_MASK	GENMASK(3, 0)
#define DATA_RATE_MODE_NON_HT		0x0
#define DATA_RATE_HT_IDX_MASK		GENMASK(4, 0)
#define DATA_RATE_HT_IDX_MASK_V1	GENMASK(4, 0)
#define DATA_RATE_MODE_HT		0x1
#define DATA_RATE_VHT_HE_NSS_MASK	GENMASK(6, 4)
#define DATA_RATE_VHT_HE_IDX_MASK	GENMASK(3, 0)
#define DATA_RATE_NSS_MASK_V1		GENMASK(7, 5)
#define DATA_RATE_MCS_MASK_V1		GENMASK(4, 0)
#define DATA_RATE_MODE_VHT		0x2
#define DATA_RATE_MODE_HE		0x3
#define DATA_RATE_MODE_EHT		0x4

static inline u8 rtw89_get_data_rate_mode(struct rtw89_dev *rtwdev, u16 hw_rate)
{
	if (rtwdev->chip->chip_gen == RTW89_CHIP_BE)
		return u16_get_bits(hw_rate, DATA_RATE_MODE_CTRL_MASK_V1);

	return u16_get_bits(hw_rate, DATA_RATE_MODE_CTRL_MASK);
}

static inline u8 rtw89_get_data_not_ht_idx(struct rtw89_dev *rtwdev, u16 hw_rate)
{
	return u16_get_bits(hw_rate, DATA_RATE_NOT_HT_IDX_MASK);
}

static inline u8 rtw89_get_data_ht_mcs(struct rtw89_dev *rtwdev, u16 hw_rate)
{
	if (rtwdev->chip->chip_gen == RTW89_CHIP_BE)
		return u16_get_bits(hw_rate, DATA_RATE_HT_IDX_MASK_V1);

	return u16_get_bits(hw_rate, DATA_RATE_HT_IDX_MASK);
}

static inline u8 rtw89_get_data_mcs(struct rtw89_dev *rtwdev, u16 hw_rate)
{
	if (rtwdev->chip->chip_gen == RTW89_CHIP_BE)
		return u16_get_bits(hw_rate, DATA_RATE_MCS_MASK_V1);

	return u16_get_bits(hw_rate, DATA_RATE_VHT_HE_IDX_MASK);
}

static inline u8 rtw89_get_data_nss(struct rtw89_dev *rtwdev, u16 hw_rate)
{
	if (rtwdev->chip->chip_gen == RTW89_CHIP_BE)
		return u16_get_bits(hw_rate, DATA_RATE_NSS_MASK_V1);

	return u16_get_bits(hw_rate, DATA_RATE_VHT_HE_NSS_MASK);
}

/* TX WD BODY DWORD 0 */
#define RTW89_TXWD_BODY0_WP_OFFSET GENMASK(31, 24)
#define RTW89_TXWD_BODY0_WP_OFFSET_V1 GENMASK(28, 24)
#define RTW89_TXWD_BODY0_MORE_DATA BIT(23)
#define RTW89_TXWD_BODY0_WD_INFO_EN BIT(22)
#define RTW89_TXWD_BODY0_FW_DL BIT(20)
#define RTW89_TXWD_BODY0_CHANNEL_DMA GENMASK(19, 16)
#define RTW89_TXWD_BODY0_HDR_LLC_LEN GENMASK(15, 11)
#define RTW89_TXWD_BODY0_WD_PAGE BIT(7)
#define RTW89_TXWD_BODY0_HW_AMSDU BIT(5)
#define RTW89_TXWD_BODY0_HW_SSN_SEL GENMASK(3, 2)
#define RTW89_TXWD_BODY0_HW_SSN_MODE GENMASK(1, 0)

/* TX WD BODY DWORD 1 */
#define RTW89_TXWD_BODY1_ADDR_INFO_NUM GENMASK(31, 26)
#define RTW89_TXWD_BODY1_PAYLOAD_ID GENMASK(31, 16)
#define RTW89_TXWD_BODY1_SEC_KEYID GENMASK(5, 4)
#define RTW89_TXWD_BODY1_SEC_TYPE GENMASK(3, 0)

/* TX WD BODY DWORD 2 */
#define RTW89_TXWD_BODY2_MACID GENMASK(30, 24)
#define RTW89_TXWD_BODY2_TID_INDICATE BIT(23)
#define RTW89_TXWD_BODY2_QSEL GENMASK(22, 17)
#define RTW89_TXWD_BODY2_TXPKT_SIZE GENMASK(13, 0)

/* TX WD BODY DWORD 3 */
#define RTW89_TXWD_BODY3_BK BIT(13)
#define RTW89_TXWD_BODY3_AGG_EN BIT(12)
#define RTW89_TXWD_BODY3_SW_SEQ GENMASK(11, 0)

/* TX WD BODY DWORD 4 */
#define RTW89_TXWD_BODY4_SEC_IV_L1 GENMASK(31, 24)
#define RTW89_TXWD_BODY4_SEC_IV_L0 GENMASK(23, 16)

/* TX WD BODY DWORD 5 */
#define RTW89_TXWD_BODY5_SEC_IV_H5 GENMASK(31, 24)
#define RTW89_TXWD_BODY5_SEC_IV_H4 GENMASK(23, 16)
#define RTW89_TXWD_BODY5_SEC_IV_H3 GENMASK(15, 8)
#define RTW89_TXWD_BODY5_SEC_IV_H2 GENMASK(7, 0)

/* TX WD BODY DWORD 6 (V1) */

/* TX WD BODY DWORD 7 (V1) */
#define RTW89_TXWD_BODY7_USE_RATE_V1 BIT(31)
#define RTW89_TXWD_BODY7_DATA_BW GENMASK(29, 28)
#define RTW89_TXWD_BODY7_GI_LTF GENMASK(27, 25)
#define RTW89_TXWD_BODY7_DATA_RATE GENMASK(24, 16)

/* TX WD INFO DWORD 0 */
#define RTW89_TXWD_INFO0_USE_RATE BIT(30)
#define RTW89_TXWD_INFO0_DATA_BW GENMASK(29, 28)
#define RTW89_TXWD_INFO0_GI_LTF GENMASK(27, 25)
#define RTW89_TXWD_INFO0_DATA_RATE GENMASK(24, 16)
#define RTW89_TXWD_INFO0_DATA_ER BIT(15)
#define RTW89_TXWD_INFO0_DISDATAFB BIT(10)
#define RTW89_TXWD_INFO0_DATA_BW_ER BIT(8)
#define RTW89_TXWD_INFO0_MULTIPORT_ID GENMASK(6, 4)

/* TX WD INFO DWORD 1 */
#define RTW89_TXWD_INFO1_DATA_RTY_LOWEST_RATE GENMASK(24, 16)
#define RTW89_TXWD_INFO1_A_CTRL_BSR BIT(14)
#define RTW89_TXWD_INFO1_MAX_AGGNUM GENMASK(7, 0)

/* TX WD INFO DWORD 2 */
#define RTW89_TXWD_INFO2_AMPDU_DENSITY GENMASK(20, 18)
#define RTW89_TXWD_INFO2_SEC_TYPE GENMASK(12, 9)
#define RTW89_TXWD_INFO2_SEC_HW_ENC BIT(8)
#define RTW89_TXWD_INFO2_FORCE_KEY_EN BIT(8)
#define RTW89_TXWD_INFO2_SEC_CAM_IDX GENMASK(7, 0)

/* TX WD INFO DWORD 3 */

/* TX WD INFO DWORD 4 */
#define RTW89_TXWD_INFO4_RTS_EN BIT(27)
#define RTW89_TXWD_INFO4_HW_RTS_EN BIT(31)

/* TX WD INFO DWORD 5 */

/* RX WD dword0 */
#define AX_RXD_RPKT_LEN_MASK GENMASK(13, 0)
#define AX_RXD_SHIFT_MASK GENMASK(15, 14)
#define AX_RXD_WL_HD_IV_LEN_MASK GENMASK(21, 16)
#define AX_RXD_BB_SEL BIT(22)
#define AX_RXD_MAC_INFO_VLD BIT(23)
#define AX_RXD_RPKT_TYPE_MASK GENMASK(27, 24)
#define AX_RXD_DRV_INFO_SIZE_MASK GENMASK(30, 28)
#define AX_RXD_LONG_RXD BIT(31)

/* RX WD dword1 */
#define AX_RXD_PPDU_TYPE_MASK GENMASK(3, 0)
#define AX_RXD_PPDU_CNT_MASK GENMASK(6, 4)
#define AX_RXD_SR_EN BIT(7)
#define AX_RXD_USER_ID_MASK GENMASK(15, 8)
#define AX_RXD_USER_ID_v1_MASK GENMASK(13, 8)
#define AX_RXD_RX_DATARATE_MASK GENMASK(24, 16)
#define AX_RXD_RX_GI_LTF_MASK GENMASK(27, 25)
#define AX_RXD_NON_SRG_PPDU BIT(28)
#define AX_RXD_INTER_PPDU BIT(29)
#define AX_RXD_NON_SRG_PPDU_v1 BIT(14)
#define AX_RXD_INTER_PPDU_v1 BIT(15)
#define AX_RXD_BW_MASK GENMASK(31, 30)
#define AX_RXD_BW_v1_MASK GENMASK(31, 29)

/* RX WD dword2 */
#define AX_RXD_FREERUN_CNT_MASK GENMASK(31, 0)

/* RX WD dword3 */
#define AX_RXD_A1_MATCH BIT(0)
#define AX_RXD_SW_DEC BIT(1)
#define AX_RXD_HW_DEC BIT(2)
#define AX_RXD_AMPDU BIT(3)
#define AX_RXD_AMPDU_END_PKT BIT(4)
#define AX_RXD_AMSDU BIT(5)
#define AX_RXD_AMSDU_CUT BIT(6)
#define AX_RXD_LAST_MSDU BIT(7)
#define AX_RXD_BYPASS BIT(8)
#define AX_RXD_CRC32_ERR BIT(9)
#define AX_RXD_ICV_ERR BIT(10)
#define AX_RXD_MAGIC_WAKE BIT(11)
#define AX_RXD_UNICAST_WAKE BIT(12)
#define AX_RXD_PATTERN_WAKE BIT(13)
#define AX_RXD_GET_CH_INFO_MASK GENMASK(15, 14)
#define AX_RXD_PATTERN_IDX_MASK GENMASK(20, 16)
#define AX_RXD_TARGET_IDC_MASK GENMASK(23, 21)
#define AX_RXD_CHKSUM_OFFLOAD_EN BIT(24)
#define AX_RXD_WITH_LLC BIT(25)
#define AX_RXD_RX_STATISTICS BIT(26)

/* RX WD dword4 */
#define AX_RXD_TYPE_MASK GENMASK(1, 0)
#define AX_RXD_MC BIT(2)
#define AX_RXD_BC BIT(3)
#define AX_RXD_MD BIT(4)
#define AX_RXD_MF BIT(5)
#define AX_RXD_PWR BIT(6)
#define AX_RXD_QOS BIT(7)
#define AX_RXD_TID_MASK GENMASK(11, 8)
#define AX_RXD_EOSP BIT(12)
#define AX_RXD_HTC BIT(13)
#define AX_RXD_QNULL BIT(14)
#define AX_RXD_SEQ_MASK GENMASK(27, 16)
#define AX_RXD_FRAG_MASK GENMASK(31, 28)

/* RX WD dword5 */
#define AX_RXD_SEC_CAM_IDX_MASK GENMASK(7, 0)
#define AX_RXD_ADDR_CAM_MASK GENMASK(15, 8)
#define AX_RXD_MAC_ID_MASK GENMASK(23, 16)
#define AX_RXD_RX_PL_ID_MASK GENMASK(27, 24)
#define AX_RXD_ADDR_CAM_VLD BIT(28)
#define AX_RXD_ADDR_FWD_EN BIT(29)
#define AX_RXD_RX_PL_MATCH BIT(30)

/* RX WD dword6 */
#define AX_RXD_MAC_ADDR_MASK GENMASK(31, 0)

/* RX WD dword7 */
#define AX_RXD_MAC_ADDR_H_MASK GENMASK(15, 0)
#define AX_RXD_SMART_ANT BIT(16)
#define AX_RXD_SEC_TYPE_MASK GENMASK(20, 17)
#define AX_RXD_HDR_CNV BIT(21)
#define AX_RXD_HDR_OFFSET_MASK GENMASK(26, 22)
#define AX_RXD_BIP_KEYID BIT(27)
#define AX_RXD_BIP_ENC BIT(28)

struct rtw89_rxinfo_user {
	__le32 w0;
};

#define RTW89_RXINFO_USER_MAC_ID_VALID BIT(0)
#define RTW89_RXINFO_USER_DATA BIT(1)
#define RTW89_RXINFO_USER_CTRL BIT(2)
#define RTW89_RXINFO_USER_MGMT BIT(3)
#define RTW89_RXINFO_USER_BCM BIT(4)
#define RTW89_RXINFO_USER_MACID GENMASK(15, 8)

struct rtw89_rxinfo {
	__le32 w0;
	__le32 w1;
	struct rtw89_rxinfo_user user[];
} __packed;

#define RTW89_RXINFO_W0_USR_NUM GENMASK(3, 0)
#define RTW89_RXINFO_W0_FW_DEFINE GENMASK(15, 8)
#define RTW89_RXINFO_W0_LSIG_LEN GENMASK(27, 16)
#define RTW89_RXINFO_W0_IS_TO_SELF BIT(28)
#define RTW89_RXINFO_W0_RX_CNT_VLD BIT(29)
#define RTW89_RXINFO_W0_LONG_RXD GENMASK(31, 30)
#define RTW89_RXINFO_W1_SERVICE GENMASK(15, 0)
#define RTW89_RXINFO_W1_PLCP_LEN GENMASK(23, 16)

struct rtw89_phy_sts_hdr {
	__le32 w0;
	__le32 w1;
} __packed;

#define RTW89_PHY_STS_HDR_W0_IE_MAP GENMASK(4, 0)
#define RTW89_PHY_STS_HDR_W0_LEN GENMASK(15, 8)
#define RTW89_PHY_STS_HDR_W0_RSSI_AVG GENMASK(31, 24)
#define RTW89_PHY_STS_HDR_W1_RSSI_A GENMASK(7, 0)
#define RTW89_PHY_STS_HDR_W1_RSSI_B GENMASK(15, 8)
#define RTW89_PHY_STS_HDR_W1_RSSI_C GENMASK(23, 16)
#define RTW89_PHY_STS_HDR_W1_RSSI_D GENMASK(31, 24)

struct rtw89_phy_sts_iehdr {
	__le32 w0;
};

#define RTW89_PHY_STS_IEHDR_TYPE GENMASK(4, 0)
#define RTW89_PHY_STS_IEHDR_LEN GENMASK(11, 5)

struct rtw89_phy_sts_ie0 {
	__le32 w0;
	__le32 w1;
	__le32 w2;
} __packed;

#define RTW89_PHY_STS_IE01_W0_CH_IDX GENMASK(23, 16)
#define RTW89_PHY_STS_IE01_W1_FD_CFO GENMASK(19, 8)
#define RTW89_PHY_STS_IE01_W1_PREMB_CFO GENMASK(31, 20)
#define RTW89_PHY_STS_IE01_W2_AVG_SNR GENMASK(5, 0)
#define RTW89_PHY_STS_IE01_W2_EVM_MAX GENMASK(15, 8)
#define RTW89_PHY_STS_IE01_W2_EVM_MIN GENMASK(23, 16)

enum rtw89_tx_channel {
	RTW89_TXCH_ACH0	= 0,
	RTW89_TXCH_ACH1	= 1,
	RTW89_TXCH_ACH2	= 2,
	RTW89_TXCH_ACH3	= 3,
	RTW89_TXCH_ACH4	= 4,
	RTW89_TXCH_ACH5	= 5,
	RTW89_TXCH_ACH6	= 6,
	RTW89_TXCH_ACH7	= 7,
	RTW89_TXCH_CH8	= 8,  /* MGMT Band 0 */
	RTW89_TXCH_CH9	= 9,  /* HI Band 0 */
	RTW89_TXCH_CH10	= 10, /* MGMT Band 1 */
	RTW89_TXCH_CH11	= 11, /* HI Band 1 */
	RTW89_TXCH_CH12	= 12, /* FW CMD */

	/* keep last */
	RTW89_TXCH_NUM,
	RTW89_TXCH_MAX = RTW89_TXCH_NUM - 1
};

enum rtw89_rx_channel {
	RTW89_RXCH_RXQ	= 0,
	RTW89_RXCH_RPQ	= 1,

	/* keep last */
	RTW89_RXCH_NUM,
	RTW89_RXCH_MAX = RTW89_RXCH_NUM - 1
};

enum rtw89_tx_qsel {
	RTW89_TX_QSEL_BE_0		= 0x00,
	RTW89_TX_QSEL_BK_0		= 0x01,
	RTW89_TX_QSEL_VI_0		= 0x02,
	RTW89_TX_QSEL_VO_0		= 0x03,
	RTW89_TX_QSEL_BE_1		= 0x04,
	RTW89_TX_QSEL_BK_1		= 0x05,
	RTW89_TX_QSEL_VI_1		= 0x06,
	RTW89_TX_QSEL_VO_1		= 0x07,
	RTW89_TX_QSEL_BE_2		= 0x08,
	RTW89_TX_QSEL_BK_2		= 0x09,
	RTW89_TX_QSEL_VI_2		= 0x0a,
	RTW89_TX_QSEL_VO_2		= 0x0b,
	RTW89_TX_QSEL_BE_3		= 0x0c,
	RTW89_TX_QSEL_BK_3		= 0x0d,
	RTW89_TX_QSEL_VI_3		= 0x0e,
	RTW89_TX_QSEL_VO_3		= 0x0f,
	RTW89_TX_QSEL_B0_BCN		= 0x10,
	RTW89_TX_QSEL_B0_HI		= 0x11,
	RTW89_TX_QSEL_B0_MGMT		= 0x12,
	RTW89_TX_QSEL_B0_NOPS		= 0x13,
	RTW89_TX_QSEL_B0_MGMT_FAST	= 0x14,
	/* reserved */
	/* reserved */
	/* reserved */
	RTW89_TX_QSEL_B1_BCN		= 0x18,
	RTW89_TX_QSEL_B1_HI		= 0x19,
	RTW89_TX_QSEL_B1_MGMT		= 0x1a,
	RTW89_TX_QSEL_B1_NOPS		= 0x1b,
	RTW89_TX_QSEL_B1_MGMT_FAST	= 0x1c,
	/* reserved */
	/* reserved */
	/* reserved */
};

static inline u8 rtw89_core_get_qsel(struct rtw89_dev *rtwdev, u8 tid)
{
	switch (tid) {
	default:
		rtw89_warn(rtwdev, "Should use tag 1d: %d\n", tid);
		fallthrough;
	case 0:
	case 3:
		return RTW89_TX_QSEL_BE_0;
	case 1:
	case 2:
		return RTW89_TX_QSEL_BK_0;
	case 4:
	case 5:
		return RTW89_TX_QSEL_VI_0;
	case 6:
	case 7:
		return RTW89_TX_QSEL_VO_0;
	}
}

static inline u8 rtw89_core_get_ch_dma(struct rtw89_dev *rtwdev, u8 qsel)
{
	switch (qsel) {
	default:
		rtw89_warn(rtwdev, "Cannot map qsel to dma: %d\n", qsel);
		fallthrough;
	case RTW89_TX_QSEL_BE_0:
		return RTW89_TXCH_ACH0;
	case RTW89_TX_QSEL_BK_0:
		return RTW89_TXCH_ACH1;
	case RTW89_TX_QSEL_VI_0:
		return RTW89_TXCH_ACH2;
	case RTW89_TX_QSEL_VO_0:
		return RTW89_TXCH_ACH3;
	case RTW89_TX_QSEL_B0_MGMT:
		return RTW89_TXCH_CH8;
	case RTW89_TX_QSEL_B0_HI:
		return RTW89_TXCH_CH9;
	case RTW89_TX_QSEL_B1_MGMT:
		return RTW89_TXCH_CH10;
	case RTW89_TX_QSEL_B1_HI:
		return RTW89_TXCH_CH11;
	}
}

static inline u8 rtw89_core_get_tid_indicate(struct rtw89_dev *rtwdev, u8 tid)
{
	switch (tid) {
	case 3:
	case 2:
	case 5:
	case 7:
		return 1;
	default:
		rtw89_warn(rtwdev, "Should use tag 1d: %d\n", tid);
		fallthrough;
	case 0:
	case 1:
	case 4:
	case 6:
		return 0;
	}
}

#endif
