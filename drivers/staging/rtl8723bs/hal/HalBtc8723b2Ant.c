// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2012 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/

#include "Mp_Precomp.h"

/* defines */
#define HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(val)			      \
do {									      \
	halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, val);           \
	pCoexDm->psTdmaDuAdjType = val;                                       \
} while (0)

/*  Global variables, these are static variables */
static struct coex_dm_8723b_2ant GLCoexDm8723b2Ant;
static struct coex_dm_8723b_2ant *pCoexDm = &GLCoexDm8723b2Ant;
static struct coex_sta_8723b_2ant GLCoexSta8723b2Ant;
static struct coex_sta_8723b_2ant *pCoexSta = &GLCoexSta8723b2Ant;

/*  local function start with halbtc8723b2ant_ */
static u8 halbtc8723b2ant_BtRssiState(
	u8 levelNum, u8 rssiThresh, u8 rssiThresh1
)
{
	s32 btRssi = 0;
	u8 btRssiState = pCoexSta->preBtRssiState;

	btRssi = pCoexSta->btRssi;

	if (levelNum == 2) {
		if (
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_LOW) ||
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_STAY_LOW)
		) {
			if (btRssi >= (rssiThresh + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				btRssiState = BTC_RSSI_STATE_HIGH;
			} else {
				btRssiState = BTC_RSSI_STATE_STAY_LOW;
			}
		} else {
			if (btRssi < rssiThresh) {
				btRssiState = BTC_RSSI_STATE_LOW;
			} else {
				btRssiState = BTC_RSSI_STATE_STAY_HIGH;
			}
		}
	} else if (levelNum == 3) {
		if (rssiThresh > rssiThresh1) {
			return pCoexSta->preBtRssiState;
		}

		if (
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_LOW) ||
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_STAY_LOW)
		) {
			if (btRssi >= (rssiThresh + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				btRssiState = BTC_RSSI_STATE_MEDIUM;
			} else {
				btRssiState = BTC_RSSI_STATE_STAY_LOW;
			}
		} else if (
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_MEDIUM) ||
			(pCoexSta->preBtRssiState == BTC_RSSI_STATE_STAY_MEDIUM)
		) {
			if (btRssi >= (rssiThresh1 + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				btRssiState = BTC_RSSI_STATE_HIGH;
			} else if (btRssi < rssiThresh) {
				btRssiState = BTC_RSSI_STATE_LOW;
			} else {
				btRssiState = BTC_RSSI_STATE_STAY_MEDIUM;
			}
		} else {
			if (btRssi < rssiThresh1) {
				btRssiState = BTC_RSSI_STATE_MEDIUM;
			} else {
				btRssiState = BTC_RSSI_STATE_STAY_HIGH;
			}
		}
	}

	pCoexSta->preBtRssiState = btRssiState;

	return btRssiState;
}

static u8 halbtc8723b2ant_WifiRssiState(
	struct btc_coexist *pBtCoexist,
	u8 index,
	u8 levelNum,
	u8 rssiThresh,
	u8 rssiThresh1
)
{
	s32 wifiRssi = 0;
	u8 wifiRssiState = pCoexSta->preWifiRssiState[index];

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_S4_WIFI_RSSI, &wifiRssi);

	if (levelNum == 2) {
		if (
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_LOW) ||
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_STAY_LOW)
		) {
			if (wifiRssi >= (rssiThresh + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				wifiRssiState = BTC_RSSI_STATE_HIGH;
			} else {
				wifiRssiState = BTC_RSSI_STATE_STAY_LOW;
			}
		} else {
			if (wifiRssi < rssiThresh) {
				wifiRssiState = BTC_RSSI_STATE_LOW;
			} else {
				wifiRssiState = BTC_RSSI_STATE_STAY_HIGH;
			}
		}
	} else if (levelNum == 3) {
		if (rssiThresh > rssiThresh1) {
			return pCoexSta->preWifiRssiState[index];
		}

		if (
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_LOW) ||
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_STAY_LOW)
		) {
			if (wifiRssi >= (rssiThresh + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				wifiRssiState = BTC_RSSI_STATE_MEDIUM;
			} else {
				wifiRssiState = BTC_RSSI_STATE_STAY_LOW;
			}
		} else if (
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_MEDIUM) ||
			(pCoexSta->preWifiRssiState[index] == BTC_RSSI_STATE_STAY_MEDIUM)
		) {
			if (wifiRssi >= (rssiThresh1 + BTC_RSSI_COEX_THRESH_TOL_8723B_2ANT)) {
				wifiRssiState = BTC_RSSI_STATE_HIGH;
			} else if (wifiRssi < rssiThresh) {
				wifiRssiState = BTC_RSSI_STATE_LOW;
			} else {
				wifiRssiState = BTC_RSSI_STATE_STAY_MEDIUM;
			}
		} else {
			if (wifiRssi < rssiThresh1) {
				wifiRssiState = BTC_RSSI_STATE_MEDIUM;
			} else {
				wifiRssiState = BTC_RSSI_STATE_STAY_HIGH;
			}
		}
	}

	pCoexSta->preWifiRssiState[index] = wifiRssiState;

	return wifiRssiState;
}

static void halbtc8723b2ant_LimitedRx(
	struct btc_coexist *pBtCoexist,
	bool bForceExec,
	bool bRejApAggPkt,
	bool bBtCtrlAggBufSize,
	u8 aggBufSize
)
{
	bool bRejectRxAgg = bRejApAggPkt;
	bool bBtCtrlRxAggSize = bBtCtrlAggBufSize;
	u8 rxAggSize = aggBufSize;

	/*  */
	/* 	Rx Aggregation related setting */
	/*  */
	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_BL_TO_REJ_AP_AGG_PKT, &bRejectRxAgg);
	/*  decide BT control aggregation buf size or not */
	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_BL_BT_CTRL_AGG_SIZE, &bBtCtrlRxAggSize);
	/*  aggregation buf size, only work when BT control Rx aggregation size. */
	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_U1_AGG_BUF_SIZE, &rxAggSize);
	/*  real update aggregation setting */
	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_AGGREGATE_CTRL, NULL);
}

static void halbtc8723b2ant_QueryBtInfo(struct btc_coexist *pBtCoexist)
{
	u8 	H2C_Parameter[1] = {0};

	pCoexSta->bC2hBtInfoReqSent = true;

	H2C_Parameter[0] |= BIT0;	/*  trigger */

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x61, 1, H2C_Parameter);
}

static bool halbtc8723b2ant_IsWifiStatusChanged(struct btc_coexist *pBtCoexist)
{
	static bool	bPreWifiBusy, bPreUnder4way, bPreBtHsOn;
	bool bWifiBusy = false, bUnder4way = false, bBtHsOn = false;
	bool bWifiConnected = false;

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_CONNECTED, &bWifiConnected);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_BUSY, &bWifiBusy);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_HS_OPERATION, &bBtHsOn);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_4_WAY_PROGRESS, &bUnder4way);

	if (bWifiConnected) {
		if (bWifiBusy != bPreWifiBusy) {
			bPreWifiBusy = bWifiBusy;
			return true;
		}

		if (bUnder4way != bPreUnder4way) {
			bPreUnder4way = bUnder4way;
			return true;
		}

		if (bBtHsOn != bPreBtHsOn) {
			bPreBtHsOn = bBtHsOn;
			return true;
		}
	}

	return false;
}

static void halbtc8723b2ant_UpdateBtLinkInfo(struct btc_coexist *pBtCoexist)
{
	struct btc_bt_link_info *pBtLinkInfo = &pBtCoexist->btLinkInfo;
	bool bBtHsOn = false;

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_HS_OPERATION, &bBtHsOn);

	pBtLinkInfo->bBtLinkExist = pCoexSta->bBtLinkExist;
	pBtLinkInfo->bScoExist = pCoexSta->bScoExist;
	pBtLinkInfo->bA2dpExist = pCoexSta->bA2dpExist;
	pBtLinkInfo->bPanExist = pCoexSta->bPanExist;
	pBtLinkInfo->bHidExist = pCoexSta->bHidExist;

	/*  work around for HS mode. */
	if (bBtHsOn) {
		pBtLinkInfo->bPanExist = true;
		pBtLinkInfo->bBtLinkExist = true;
	}

	/*  check if Sco only */
	if (
		pBtLinkInfo->bScoExist &&
		!pBtLinkInfo->bA2dpExist &&
		!pBtLinkInfo->bPanExist &&
		!pBtLinkInfo->bHidExist
	)
		pBtLinkInfo->bScoOnly = true;
	else
		pBtLinkInfo->bScoOnly = false;

	/*  check if A2dp only */
	if (
		!pBtLinkInfo->bScoExist &&
		pBtLinkInfo->bA2dpExist &&
		!pBtLinkInfo->bPanExist &&
		!pBtLinkInfo->bHidExist
	)
		pBtLinkInfo->bA2dpOnly = true;
	else
		pBtLinkInfo->bA2dpOnly = false;

	/*  check if Pan only */
	if (
		!pBtLinkInfo->bScoExist &&
		!pBtLinkInfo->bA2dpExist &&
		pBtLinkInfo->bPanExist &&
		!pBtLinkInfo->bHidExist
	)
		pBtLinkInfo->bPanOnly = true;
	else
		pBtLinkInfo->bPanOnly = false;

	/*  check if Hid only */
	if (
		!pBtLinkInfo->bScoExist &&
		!pBtLinkInfo->bA2dpExist &&
		!pBtLinkInfo->bPanExist &&
		pBtLinkInfo->bHidExist
	)
		pBtLinkInfo->bHidOnly = true;
	else
		pBtLinkInfo->bHidOnly = false;
}

static u8 halbtc8723b2ant_ActionAlgorithm(struct btc_coexist *pBtCoexist)
{
	struct btc_bt_link_info *pBtLinkInfo = &pBtCoexist->btLinkInfo;
	bool bBtHsOn = false;
	u8 algorithm = BT_8723B_2ANT_COEX_ALGO_UNDEFINED;
	u8 numOfDiffProfile = 0;

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_HS_OPERATION, &bBtHsOn);

	if (!pBtLinkInfo->bBtLinkExist) {
		return algorithm;
	}

	if (pBtLinkInfo->bScoExist)
		numOfDiffProfile++;

	if (pBtLinkInfo->bHidExist)
		numOfDiffProfile++;

	if (pBtLinkInfo->bPanExist)
		numOfDiffProfile++;

	if (pBtLinkInfo->bA2dpExist)
		numOfDiffProfile++;

	if (numOfDiffProfile == 1) {
		if (pBtLinkInfo->bScoExist) {
			algorithm = BT_8723B_2ANT_COEX_ALGO_SCO;
		} else {
			if (pBtLinkInfo->bHidExist) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_HID;
			} else if (pBtLinkInfo->bA2dpExist) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_A2DP;
			} else if (pBtLinkInfo->bPanExist) {
				if (bBtHsOn) {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANHS;
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR;
				}
			}
		}
	} else if (numOfDiffProfile == 2) {
		if (pBtLinkInfo->bScoExist) {
			if (pBtLinkInfo->bHidExist) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
			} else if (pBtLinkInfo->bA2dpExist) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
			} else if (pBtLinkInfo->bPanExist) {
				if (bBtHsOn) {
					algorithm = BT_8723B_2ANT_COEX_ALGO_SCO;
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
				}
			}
		} else {
			if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bA2dpExist
			) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_HID_A2DP;
			} else if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bPanExist
			) {
				if (bBtHsOn) {
					algorithm = BT_8723B_2ANT_COEX_ALGO_HID;
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
				}
			} else if (
				pBtLinkInfo->bPanExist &&
				pBtLinkInfo->bA2dpExist
			) {
				if (bBtHsOn) {
					algorithm = BT_8723B_2ANT_COEX_ALGO_A2DP_PANHS;
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_A2DP;
				}
			}
		}
	} else if (numOfDiffProfile == 3) {
		if (pBtLinkInfo->bScoExist) {
			if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bA2dpExist
			) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
			} else if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bPanExist
			) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
			} else if (
				pBtLinkInfo->bPanExist &&
				pBtLinkInfo->bA2dpExist
			) {
				algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
			}
		} else {
			if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bPanExist &&
				pBtLinkInfo->bA2dpExist
			) {
				if (bBtHsOn) {
					algorithm = BT_8723B_2ANT_COEX_ALGO_HID_A2DP;
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_HID_A2DP_PANEDR;
				}
			}
		}
	} else if (numOfDiffProfile >= 3) {
		if (pBtLinkInfo->bScoExist) {
			if (
				pBtLinkInfo->bHidExist &&
				pBtLinkInfo->bPanExist &&
				pBtLinkInfo->bA2dpExist
			) {
				if (bBtHsOn) {
				} else {
					algorithm = BT_8723B_2ANT_COEX_ALGO_PANEDR_HID;
				}
			}
		}
	}

	return algorithm;
}

static void halbtc8723b2ant_SetFwDacSwingLevel(
	struct btc_coexist *pBtCoexist, u8 dacSwingLvl
)
{
	u8 	H2C_Parameter[1] = {0};

	/*  There are several type of dacswing */
	/*  0x18/ 0x10/ 0xc/ 0x8/ 0x4/ 0x6 */
	H2C_Parameter[0] = dacSwingLvl;

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x64, 1, H2C_Parameter);
}

static void halbtc8723b2ant_SetFwDecBtPwr(
	struct btc_coexist *pBtCoexist, u8 decBtPwrLvl
)
{
	u8 	H2C_Parameter[1] = {0};

	H2C_Parameter[0] = decBtPwrLvl;

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x62, 1, H2C_Parameter);
}

static void halbtc8723b2ant_DecBtPwr(
	struct btc_coexist *pBtCoexist, bool bForceExec, u8 decBtPwrLvl
)
{
	pCoexDm->curBtDecPwrLvl = decBtPwrLvl;

	if (!bForceExec) {
		if (pCoexDm->preBtDecPwrLvl == pCoexDm->curBtDecPwrLvl)
			return;
	}
	halbtc8723b2ant_SetFwDecBtPwr(pBtCoexist, pCoexDm->curBtDecPwrLvl);

	pCoexDm->preBtDecPwrLvl = pCoexDm->curBtDecPwrLvl;
}

static void halbtc8723b2ant_FwDacSwingLvl(
	struct btc_coexist *pBtCoexist, bool bForceExec, u8 fwDacSwingLvl
)
{
	pCoexDm->curFwDacSwingLvl = fwDacSwingLvl;

	if (!bForceExec) {
		if (pCoexDm->preFwDacSwingLvl == pCoexDm->curFwDacSwingLvl)
			return;
	}

	halbtc8723b2ant_SetFwDacSwingLevel(pBtCoexist, pCoexDm->curFwDacSwingLvl);

	pCoexDm->preFwDacSwingLvl = pCoexDm->curFwDacSwingLvl;
}

static void halbtc8723b2ant_SetSwRfRxLpfCorner(
	struct btc_coexist *pBtCoexist,
	bool bRxRfShrinkOn
)
{
	if (bRxRfShrinkOn) {
		/* Shrink RF Rx LPF corner */
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1e, 0xfffff, 0xffffc);
	} else {
		/* Resume RF Rx LPF corner */
		/*  After initialized, we can use pCoexDm->btRf0x1eBackup */
		if (pBtCoexist->bInitilized) {
			pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1e, 0xfffff, pCoexDm->btRf0x1eBackup);
		}
	}
}

static void halbtc8723b2ant_RfShrink(
	struct btc_coexist *pBtCoexist, bool bForceExec, bool bRxRfShrinkOn
)
{
	pCoexDm->bCurRfRxLpfShrink = bRxRfShrinkOn;

	if (!bForceExec) {
		if (pCoexDm->bPreRfRxLpfShrink == pCoexDm->bCurRfRxLpfShrink)
			return;
	}
	halbtc8723b2ant_SetSwRfRxLpfCorner(pBtCoexist, pCoexDm->bCurRfRxLpfShrink);

	pCoexDm->bPreRfRxLpfShrink = pCoexDm->bCurRfRxLpfShrink;
}

static void halbtc8723b2ant_SetSwPenaltyTxRateAdaptive(
	struct btc_coexist *pBtCoexist, bool bLowPenaltyRa
)
{
	u8 	H2C_Parameter[6] = {0};

	H2C_Parameter[0] = 0x6;	/*  opCode, 0x6 = Retry_Penalty */

	if (bLowPenaltyRa) {
		H2C_Parameter[1] |= BIT0;
		H2C_Parameter[2] = 0x00;  /* normal rate except MCS7/6/5, OFDM54/48/36 */
		H2C_Parameter[3] = 0xf7;  /* MCS7 or OFDM54 */
		H2C_Parameter[4] = 0xf8;  /* MCS6 or OFDM48 */
		H2C_Parameter[5] = 0xf9;	/* MCS5 or OFDM36 */
	}

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x69, 6, H2C_Parameter);
}

static void halbtc8723b2ant_LowPenaltyRa(
	struct btc_coexist *pBtCoexist, bool bForceExec, bool bLowPenaltyRa
)
{
	/* return; */
	pCoexDm->bCurLowPenaltyRa = bLowPenaltyRa;

	if (!bForceExec) {
		if (pCoexDm->bPreLowPenaltyRa == pCoexDm->bCurLowPenaltyRa)
			return;
	}
	halbtc8723b2ant_SetSwPenaltyTxRateAdaptive(pBtCoexist, pCoexDm->bCurLowPenaltyRa);

	pCoexDm->bPreLowPenaltyRa = pCoexDm->bCurLowPenaltyRa;
}

static void halbtc8723b2ant_SetDacSwingReg(struct btc_coexist *pBtCoexist, u32 level)
{
	u8 val = (u8)level;

	pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x883, 0x3e, val);
}

static void halbtc8723b2ant_SetSwFullTimeDacSwing(
	struct btc_coexist *pBtCoexist, bool bSwDacSwingOn, u32 swDacSwingLvl
)
{
	if (bSwDacSwingOn)
		halbtc8723b2ant_SetDacSwingReg(pBtCoexist, swDacSwingLvl);
	else
		halbtc8723b2ant_SetDacSwingReg(pBtCoexist, 0x18);
}


static void halbtc8723b2ant_DacSwing(
	struct btc_coexist *pBtCoexist,
	bool bForceExec,
	bool bDacSwingOn,
	u32 dacSwingLvl
)
{
	pCoexDm->bCurDacSwingOn = bDacSwingOn;
	pCoexDm->curDacSwingLvl = dacSwingLvl;

	if (!bForceExec) {
		if ((pCoexDm->bPreDacSwingOn == pCoexDm->bCurDacSwingOn) &&
			(pCoexDm->preDacSwingLvl == pCoexDm->curDacSwingLvl))
			return;
	}
	mdelay(30);
	halbtc8723b2ant_SetSwFullTimeDacSwing(pBtCoexist, bDacSwingOn, dacSwingLvl);

	pCoexDm->bPreDacSwingOn = pCoexDm->bCurDacSwingOn;
	pCoexDm->preDacSwingLvl = pCoexDm->curDacSwingLvl;
}

static void halbtc8723b2ant_SetAgcTable(
	struct btc_coexist *pBtCoexist, bool bAgcTableEn
)
{
	u8 rssiAdjustVal = 0;

	/* BB AGC Gain Table */
	if (bAgcTableEn) {
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x6e1A0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x6d1B0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x6c1C0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x6b1D0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x6a1E0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x691F0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0x68200001);
	} else {
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xaa1A0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa91B0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa81C0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa71D0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa61E0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa51F0001);
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0xc78, 0xa4200001);
	}


	/* RF Gain */
	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0xef, 0xfffff, 0x02000);
	if (bAgcTableEn) {
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x3b, 0xfffff, 0x38fff);
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x3b, 0xfffff, 0x38ffe);
	} else {
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x3b, 0xfffff, 0x380c3);
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x3b, 0xfffff, 0x28ce6);
	}
	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0xef, 0xfffff, 0x0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0xed, 0xfffff, 0x1);
	if (bAgcTableEn) {
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x40, 0xfffff, 0x38fff);
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x40, 0xfffff, 0x38ffe);
	} else {
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x40, 0xfffff, 0x380c3);
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x40, 0xfffff, 0x28ce6);
	}
	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0xed, 0xfffff, 0x0);

	/*  set rssiAdjustVal for wifi module. */
	if (bAgcTableEn)
		rssiAdjustVal = 8;

	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_U1_RSSI_ADJ_VAL_FOR_AGC_TABLE_ON, &rssiAdjustVal);
}

static void halbtc8723b2ant_AgcTable(
	struct btc_coexist *pBtCoexist, bool bForceExec, bool bAgcTableEn
)
{
	pCoexDm->bCurAgcTableEn = bAgcTableEn;

	if (!bForceExec) {
		if (pCoexDm->bPreAgcTableEn == pCoexDm->bCurAgcTableEn)
			return;
	}
	halbtc8723b2ant_SetAgcTable(pBtCoexist, bAgcTableEn);

	pCoexDm->bPreAgcTableEn = pCoexDm->bCurAgcTableEn;
}

static void halbtc8723b2ant_SetCoexTable(
	struct btc_coexist *pBtCoexist,
	u32 val0x6c0,
	u32 val0x6c4,
	u32 val0x6c8,
	u8 val0x6cc
)
{
	pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x6c0, val0x6c0);

	pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x6c4, val0x6c4);

	pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x6c8, val0x6c8);

	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x6cc, val0x6cc);
}

static void halbtc8723b2ant_CoexTable(
	struct btc_coexist *pBtCoexist,
	bool bForceExec,
	u32 val0x6c0,
	u32 val0x6c4,
	u32 val0x6c8,
	u8 val0x6cc
)
{
	pCoexDm->curVal0x6c0 = val0x6c0;
	pCoexDm->curVal0x6c4 = val0x6c4;
	pCoexDm->curVal0x6c8 = val0x6c8;
	pCoexDm->curVal0x6cc = val0x6cc;

	if (!bForceExec) {
		if (
			(pCoexDm->preVal0x6c0 == pCoexDm->curVal0x6c0) &&
			(pCoexDm->preVal0x6c4 == pCoexDm->curVal0x6c4) &&
			(pCoexDm->preVal0x6c8 == pCoexDm->curVal0x6c8) &&
			(pCoexDm->preVal0x6cc == pCoexDm->curVal0x6cc)
		)
			return;
	}
	halbtc8723b2ant_SetCoexTable(pBtCoexist, val0x6c0, val0x6c4, val0x6c8, val0x6cc);

	pCoexDm->preVal0x6c0 = pCoexDm->curVal0x6c0;
	pCoexDm->preVal0x6c4 = pCoexDm->curVal0x6c4;
	pCoexDm->preVal0x6c8 = pCoexDm->curVal0x6c8;
	pCoexDm->preVal0x6cc = pCoexDm->curVal0x6cc;
}

static void halbtc8723b2ant_CoexTableWithType(
	struct btc_coexist *pBtCoexist, bool bForceExec, u8 type
)
{
	switch (type) {
	case 0:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55555555, 0x55555555, 0xffff, 0x3);
		break;
	case 1:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55555555, 0x5afa5afa, 0xffff, 0x3);
		break;
	case 2:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x5a5a5a5a, 0x5a5a5a5a, 0xffff, 0x3);
		break;
	case 3:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0xaaaaaaaa, 0xaaaaaaaa, 0xffff, 0x3);
		break;
	case 4:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0xffffffff, 0xffffffff, 0xffff, 0x3);
		break;
	case 5:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x5fff5fff, 0x5fff5fff, 0xffff, 0x3);
		break;
	case 6:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0x5a5a5a5a, 0xffff, 0x3);
		break;
	case 7:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0xfafafafa, 0xffff, 0x3);
		break;
	case 8:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x5aea5aea, 0x5aea5aea, 0xffff, 0x3);
		break;
	case 9:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0x5aea5aea, 0xffff, 0x3);
		break;
	case 10:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0x5aff5aff, 0xffff, 0x3);
		break;
	case 11:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0x5a5f5a5f, 0xffff, 0x3);
		break;
	case 12:
		halbtc8723b2ant_CoexTable(pBtCoexist, bForceExec, 0x55ff55ff, 0x5f5f5f5f, 0xffff, 0x3);
		break;
	default:
		break;
	}
}

static void halbtc8723b2ant_SetFwIgnoreWlanAct(
	struct btc_coexist *pBtCoexist, bool bEnable
)
{
	u8 	H2C_Parameter[1] = {0};

	if (bEnable)
		H2C_Parameter[0] |= BIT0;		/*  function enable */

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x63, 1, H2C_Parameter);
}

static void halbtc8723b2ant_IgnoreWlanAct(
	struct btc_coexist *pBtCoexist, bool bForceExec, bool bEnable
)
{
	pCoexDm->bCurIgnoreWlanAct = bEnable;

	if (!bForceExec) {
		if (pCoexDm->bPreIgnoreWlanAct == pCoexDm->bCurIgnoreWlanAct)
			return;
	}
	halbtc8723b2ant_SetFwIgnoreWlanAct(pBtCoexist, bEnable);

	pCoexDm->bPreIgnoreWlanAct = pCoexDm->bCurIgnoreWlanAct;
}

static void halbtc8723b2ant_SetFwPstdma(
	struct btc_coexist *pBtCoexist,
	u8 byte1,
	u8 byte2,
	u8 byte3,
	u8 byte4,
	u8 byte5
)
{
	u8 	H2C_Parameter[5] = {0};

	H2C_Parameter[0] = byte1;
	H2C_Parameter[1] = byte2;
	H2C_Parameter[2] = byte3;
	H2C_Parameter[3] = byte4;
	H2C_Parameter[4] = byte5;

	pCoexDm->psTdmaPara[0] = byte1;
	pCoexDm->psTdmaPara[1] = byte2;
	pCoexDm->psTdmaPara[2] = byte3;
	pCoexDm->psTdmaPara[3] = byte4;
	pCoexDm->psTdmaPara[4] = byte5;

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x60, 5, H2C_Parameter);
}

static void halbtc8723b2ant_SwMechanism1(
	struct btc_coexist *pBtCoexist,
	bool bShrinkRxLPF,
	bool bLowPenaltyRA,
	bool bLimitedDIG,
	bool bBTLNAConstrain
)
{
	halbtc8723b2ant_RfShrink(pBtCoexist, NORMAL_EXEC, bShrinkRxLPF);
	halbtc8723b2ant_LowPenaltyRa(pBtCoexist, NORMAL_EXEC, bLowPenaltyRA);
}

static void halbtc8723b2ant_SwMechanism2(
	struct btc_coexist *pBtCoexist,
	bool bAGCTableShift,
	bool bADCBackOff,
	bool bSWDACSwing,
	u32 dacSwingLvl
)
{
	halbtc8723b2ant_AgcTable(pBtCoexist, NORMAL_EXEC, bAGCTableShift);
	halbtc8723b2ant_DacSwing(pBtCoexist, NORMAL_EXEC, bSWDACSwing, dacSwingLvl);
}

static void halbtc8723b2ant_SetAntPath(
	struct btc_coexist *pBtCoexist, u8 antPosType, bool bInitHwCfg, bool bWifiOff
)
{
	struct btc_board_info *pBoardInfo = &pBtCoexist->boardInfo;
	u32 fwVer = 0, u4Tmp = 0;
	bool bPgExtSwitch = false;
	bool bUseExtSwitch = false;
	u8 	H2C_Parameter[2] = {0};

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_EXT_SWITCH, &bPgExtSwitch);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_FW_VER, &fwVer);	/*  [31:16]=fw ver, [15:0]=fw sub ver */

	if ((fwVer > 0 && fwVer < 0xc0000) || bPgExtSwitch)
		bUseExtSwitch = true;

	if (bInitHwCfg) {
		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x39, 0x8, 0x1);
		pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x974, 0xff);
		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x944, 0x3, 0x3);
		pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x930, 0x77);
		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x67, 0x20, 0x1);

		if (fwVer >= 0x180000) {
			/* Use H2C to set GNT_BT to LOW */
			H2C_Parameter[0] = 0;
			pBtCoexist->fBtcFillH2c(pBtCoexist, 0x6E, 1, H2C_Parameter);
		} else {
			pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x765, 0x0);
		}

		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x0);

		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0); /* WiFi TRx Mask off */
		pBtCoexist->fBtcSetBtReg(pBtCoexist, BTC_BT_REG_RF, 0x3c, 0x01); /* BT TRx Mask off */

		if (pBoardInfo->btdmAntPos == BTC_ANTENNA_AT_MAIN_PORT) {
			/* tell firmware "no antenna inverse" */
			H2C_Parameter[0] = 0;
		} else {
			/* tell firmware "antenna inverse" */
			H2C_Parameter[0] = 1;
		}

		if (bUseExtSwitch) {
			/* ext switch type */
			H2C_Parameter[1] = 1;
		} else {
			/* int switch type */
			H2C_Parameter[1] = 0;
		}
		pBtCoexist->fBtcFillH2c(pBtCoexist, 0x65, 2, H2C_Parameter);
	}

	/*  ext switch setting */
	if (bUseExtSwitch) {
		if (bInitHwCfg) {
			/*  0x4c[23]= 0, 0x4c[24]= 1  Antenna control by WL/BT */
			u4Tmp = pBtCoexist->fBtcRead4Byte(pBtCoexist, 0x4c);
			u4Tmp &= ~BIT23;
			u4Tmp |= BIT24;
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x4c, u4Tmp);
		}

		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x0); /*  fixed internal switch S1->WiFi, S0->BT */
		switch (antPosType) {
		case BTC_ANT_WIFI_AT_MAIN:
			pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x92c, 0x3, 0x1);	/*  ext switch main at wifi */
			break;
		case BTC_ANT_WIFI_AT_AUX:
			pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x92c, 0x3, 0x2);	/*  ext switch aux at wifi */
			break;
		}
	} else { /*  internal switch */
		if (bInitHwCfg) {
			/*  0x4c[23]= 0, 0x4c[24]= 1  Antenna control by WL/BT */
			u4Tmp = pBtCoexist->fBtcRead4Byte(pBtCoexist, 0x4c);
			u4Tmp |= BIT23;
			u4Tmp &= ~BIT24;
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x4c, u4Tmp);
		}

		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x64, 0x1, 0x0); /* fixed external switch S1->Main, S0->Aux */
		switch (antPosType) {
		case BTC_ANT_WIFI_AT_MAIN:
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x0); /*  fixed internal switch S1->WiFi, S0->BT */
			break;
		case BTC_ANT_WIFI_AT_AUX:
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x280); /*  fixed internal switch S0->WiFi, S1->BT */
			break;
		}
	}
}

static void halbtc8723b2ant_PsTdma(
	struct btc_coexist *pBtCoexist, bool bForceExec, bool bTurnOn, u8 type
)
{
	pCoexDm->bCurPsTdmaOn = bTurnOn;
	pCoexDm->curPsTdma = type;

	if (!bForceExec) {
		if (
			(pCoexDm->bPrePsTdmaOn == pCoexDm->bCurPsTdmaOn) &&
			(pCoexDm->prePsTdma == pCoexDm->curPsTdma)
		)
			return;
	}

	if (bTurnOn) {
		switch (type) {
		case 1:
		default:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1a, 0x1a, 0xe1, 0x90);
			break;
		case 2:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x12, 0x12, 0xe1, 0x90);
			break;
		case 3:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1c, 0x3, 0xf1, 0x90);
			break;
		case 4:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x10, 0x03, 0xf1, 0x90);
			break;
		case 5:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1a, 0x1a, 0x60, 0x90);
			break;
		case 6:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x12, 0x12, 0x60, 0x90);
			break;
		case 7:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1c, 0x3, 0x70, 0x90);
			break;
		case 8:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xa3, 0x10, 0x3, 0x70, 0x90);
			break;
		case 9:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1a, 0x1a, 0xe1, 0x90);
			break;
		case 10:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x12, 0x12, 0xe1, 0x90);
			break;
		case 11:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0xa, 0xa, 0xe1, 0x90);
			break;
		case 12:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x5, 0x5, 0xe1, 0x90);
			break;
		case 13:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1a, 0x1a, 0x60, 0x90);
			break;
		case 14:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x12, 0x12, 0x60, 0x90);
			break;
		case 15:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0xa, 0xa, 0x60, 0x90);
			break;
		case 16:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x5, 0x5, 0x60, 0x90);
			break;
		case 17:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xa3, 0x2f, 0x2f, 0x60, 0x90);
			break;
		case 18:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x5, 0x5, 0xe1, 0x90);
			break;
		case 19:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x25, 0x25, 0xe1, 0x90);
			break;
		case 20:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x25, 0x25, 0x60, 0x90);
			break;
		case 21:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x15, 0x03, 0x70, 0x90);
			break;
		case 71:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0xe3, 0x1a, 0x1a, 0xe1, 0x90);
			break;
		}
	} else {
		/*  disable PS tdma */
		switch (type) {
		case 0:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0x0, 0x0, 0x0, 0x40, 0x0);
			break;
		case 1:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0x0, 0x0, 0x0, 0x48, 0x0);
			break;
		default:
			halbtc8723b2ant_SetFwPstdma(pBtCoexist, 0x0, 0x0, 0x0, 0x40, 0x0);
			break;
		}
	}

	/*  update pre state */
	pCoexDm->bPrePsTdmaOn = pCoexDm->bCurPsTdmaOn;
	pCoexDm->prePsTdma = pCoexDm->curPsTdma;
}

static void halbtc8723b2ant_CoexAllOff(struct btc_coexist *pBtCoexist)
{
	/*  fw all off */
	halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);
	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);
	halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	/*  sw all off */
	halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
	halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);

	/*  hw all off */
	/* pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0); */
	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
}

static void halbtc8723b2ant_InitCoexDm(struct btc_coexist *pBtCoexist)
{
	/*  force to reset coex mechanism */

	halbtc8723b2ant_PsTdma(pBtCoexist, FORCE_EXEC, false, 1);
	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, FORCE_EXEC, 6);
	halbtc8723b2ant_DecBtPwr(pBtCoexist, FORCE_EXEC, 0);

	halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
	halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
}

static void halbtc8723b2ant_ActionBtInquiry(struct btc_coexist *pBtCoexist)
{
	bool bWifiConnected = false;
	bool bLowPwrDisable = true;

	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_DISABLE_LOW_POWER, &bLowPwrDisable);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_CONNECTED, &bWifiConnected);

	if (bWifiConnected) {
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 3);
	} else {
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);
	}

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, FORCE_EXEC, 6);
	halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
	halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);

	pCoexDm->bNeedRecover0x948 = true;
	pCoexDm->backup0x948 = pBtCoexist->fBtcRead4Byte(pBtCoexist, 0x948);

	halbtc8723b2ant_SetAntPath(pBtCoexist, BTC_ANT_WIFI_AT_AUX, false, false);
}

static bool halbtc8723b2ant_IsCommonAction(struct btc_coexist *pBtCoexist)
{
	u8 btRssiState = BTC_RSSI_STATE_HIGH;
	bool bCommon = false, bWifiConnected = false, bWifiBusy = false;
	bool bBtHsOn = false, bLowPwrDisable = false;

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_HS_OPERATION, &bBtHsOn);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_CONNECTED, &bWifiConnected);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_BUSY, &bWifiBusy);

	if (!bWifiConnected) {
		bLowPwrDisable = false;
		pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_DISABLE_LOW_POWER, &bLowPwrDisable);
		halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);
		halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

		halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
		halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);

		bCommon = true;
	} else {
		if (BT_8723B_2ANT_BT_STATUS_NON_CONNECTED_IDLE == pCoexDm->btStatus) {
			bLowPwrDisable = false;
			pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_DISABLE_LOW_POWER, &bLowPwrDisable);
			halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

			pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
			halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
			halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);
			halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 0xb);
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);

			bCommon = true;
		} else if (BT_8723B_2ANT_BT_STATUS_CONNECTED_IDLE == pCoexDm->btStatus) {
			bLowPwrDisable = true;
			pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_DISABLE_LOW_POWER, &bLowPwrDisable);

			if (bBtHsOn)
				return false;

			halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

			pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
			halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
			halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);
			halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 0xb);
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);

			bCommon = true;
		} else {
			bLowPwrDisable = true;
			pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_ACT_DISABLE_LOW_POWER, &bLowPwrDisable);

			if (bWifiBusy) {
				bCommon = false;
			} else {
				if (bBtHsOn)
					return false;

				btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);
				halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

				pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
				halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);
				halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 21);
				halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 0xb);

				if (BTC_RSSI_HIGH(btRssiState))
					halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
				else
					halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

				halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
				halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
				bCommon = true;
			}
		}
	}

	return bCommon;
}

static void halbtc8723b2ant_TdmaDurationAdjust(
	struct btc_coexist *pBtCoexist, bool bScoHid, bool bTxPause, u8 maxInterval
)
{
	static s32 up, dn, m, n, WaitCount;
	s32 result;   /* 0: no change, +1: increase WiFi duration, -1: decrease WiFi duration */
	u8 retryCount = 0;

	if (!pCoexDm->bAutoTdmaAdjust) {
		pCoexDm->bAutoTdmaAdjust = true;
		{
			if (bScoHid) {
				if (bTxPause) {
					if (maxInterval == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(13);
					else if (maxInterval == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
					else
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				} else {
					if (maxInterval == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(9);
					else if (maxInterval == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
					else
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				}
			} else {
				if (bTxPause) {
					if (maxInterval == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(5);
					else if (maxInterval == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				} else {
					if (maxInterval == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(1);
					else if (maxInterval == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				}
			}
		}
		/*  */
		up = 0;
		dn = 0;
		m = 1;
		n = 3;
		result = 0;
		WaitCount = 0;
	} else {
		/* acquire the BT TRx retry count from BT_Info byte2 */
		retryCount = pCoexSta->btRetryCnt;
		result = 0;
		WaitCount++;

		if (retryCount == 0) { /*  no retry in the last 2-second duration */
			up++;
			dn--;

			if (dn <= 0)
				dn = 0;

			if (up >= n) { /*  if 連續 n 個2秒 retry count為0, 則調寬WiFi duration */
				WaitCount = 0;
				n = 3;
				up = 0;
				dn = 0;
				result = 1;
			}
		} else if (retryCount <= 3) { /*  <=3 retry in the last 2-second duration */
			up--;
			dn++;

			if (up <= 0)
				up = 0;

			if (dn == 2) { /*  if 連續 2 個2秒 retry count< 3, 則調窄WiFi duration */
				if (WaitCount <= 2)
					m++; /*  避免一直在兩個level中來回 */
				else
					m = 1;

				if (m >= 20) /* m 最大值 = 20 ' 最大120秒 recheck是否調整 WiFi duration. */
					m = 20;

				n = 3 * m;
				up = 0;
				dn = 0;
				WaitCount = 0;
				result = -1;
			}
		} else { /* retry count > 3, 只要1次 retry count > 3, 則調窄WiFi duration */
			if (WaitCount == 1)
				m++; /*  避免一直在兩個level中來回 */
			else
				m = 1;

			if (m >= 20) /* m 最大值 = 20 ' 最大120秒 recheck是否調整 WiFi duration. */
				m = 20;

			n = 3 * m;
			up = 0;
			dn = 0;
			WaitCount = 0;
			result = -1;
		}

		if (maxInterval == 1) {
			if (bTxPause) {
				if (pCoexDm->curPsTdma == 71)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(5);
				else if (pCoexDm->curPsTdma == 1)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(5);
				else if (pCoexDm->curPsTdma == 2)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
				else if (pCoexDm->curPsTdma == 3)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				else if (pCoexDm->curPsTdma == 4)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);

				if (pCoexDm->curPsTdma == 9)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(13);
				else if (pCoexDm->curPsTdma == 10)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
				else if (pCoexDm->curPsTdma == 11)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				else if (pCoexDm->curPsTdma == 12)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 5)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);
					else if (pCoexDm->curPsTdma == 13)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 8)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(5);
					else if (pCoexDm->curPsTdma == 16)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(13);
				}
			} else {
				if (pCoexDm->curPsTdma == 5)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(71);
				else if (pCoexDm->curPsTdma == 6)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
				else if (pCoexDm->curPsTdma == 7)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				else if (pCoexDm->curPsTdma == 8)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);

				if (pCoexDm->curPsTdma == 13)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(9);
				else if (pCoexDm->curPsTdma == 14)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
				else if (pCoexDm->curPsTdma == 15)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				else if (pCoexDm->curPsTdma == 16)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 71)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(1);
					else if (pCoexDm->curPsTdma == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);
					else if (pCoexDm->curPsTdma == 9)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 4)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(1);
					else if (pCoexDm->curPsTdma == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(71);
					else if (pCoexDm->curPsTdma == 12)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(9);
				}
			}
		} else if (maxInterval == 2) {
			if (bTxPause) {
				if (pCoexDm->curPsTdma == 1)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
				else if (pCoexDm->curPsTdma == 2)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
				else if (pCoexDm->curPsTdma == 3)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				else if (pCoexDm->curPsTdma == 4)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);

				if (pCoexDm->curPsTdma == 9)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
				else if (pCoexDm->curPsTdma == 10)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
				else if (pCoexDm->curPsTdma == 11)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				else if (pCoexDm->curPsTdma == 12)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 5)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);
					else if (pCoexDm->curPsTdma == 13)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 8)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(6);
					else if (pCoexDm->curPsTdma == 16)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(14);
				}
			} else {
				if (pCoexDm->curPsTdma == 5)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
				else if (pCoexDm->curPsTdma == 6)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
				else if (pCoexDm->curPsTdma == 7)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				else if (pCoexDm->curPsTdma == 8)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);

				if (pCoexDm->curPsTdma == 13)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
				else if (pCoexDm->curPsTdma == 14)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
				else if (pCoexDm->curPsTdma == 15)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				else if (pCoexDm->curPsTdma == 16)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);
					else if (pCoexDm->curPsTdma == 9)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 4)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(2);
					else if (pCoexDm->curPsTdma == 12)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(10);
				}
			}
		} else if (maxInterval == 3) {
			if (bTxPause) {
				if (pCoexDm->curPsTdma == 1)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				else if (pCoexDm->curPsTdma == 2)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				else if (pCoexDm->curPsTdma == 3)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
				else if (pCoexDm->curPsTdma == 4)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);

				if (pCoexDm->curPsTdma == 9)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				else if (pCoexDm->curPsTdma == 10)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				else if (pCoexDm->curPsTdma == 11)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				else if (pCoexDm->curPsTdma == 12)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 5)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(8);
					else if (pCoexDm->curPsTdma == 13)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(16);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 8)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 7)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 6)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(7);
					else if (pCoexDm->curPsTdma == 16)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 15)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
					else if (pCoexDm->curPsTdma == 14)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(15);
				}
			} else {
				if (pCoexDm->curPsTdma == 5)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				else if (pCoexDm->curPsTdma == 6)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				else if (pCoexDm->curPsTdma == 7)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
				else if (pCoexDm->curPsTdma == 8)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);

				if (pCoexDm->curPsTdma == 13)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				else if (pCoexDm->curPsTdma == 14)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				else if (pCoexDm->curPsTdma == 15)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				else if (pCoexDm->curPsTdma == 16)
					HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);

				if (result == -1) {
					if (pCoexDm->curPsTdma == 1)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(4);
					else if (pCoexDm->curPsTdma == 9)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(12);
				} else if (result == 1) {
					if (pCoexDm->curPsTdma == 4)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 3)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 2)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(3);
					else if (pCoexDm->curPsTdma == 12)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 11)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
					else if (pCoexDm->curPsTdma == 10)
						HAL_BTC8723B2ANT_DMA_DURATION_ADJUST(11);
				}
			}
		}
	}

	/*  if current PsTdma not match with the recorded one (when scan, dhcp...), */
	/*  then we have to adjust it back to the previous record one. */
	if (pCoexDm->curPsTdma != pCoexDm->psTdmaDuAdjType) {
		bool bScan = false, bLink = false, bRoam = false;

		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_SCAN, &bScan);
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_LINK, &bLink);
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_ROAM, &bRoam);

		if (!bScan && !bLink && !bRoam)
			halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, pCoexDm->psTdmaDuAdjType);

	}
}

/*  SCO only or SCO+PAN(HS) */
static void halbtc8723b2ant_ActionSco(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 4);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);

	if (BTC_WIFI_BW_LEGACY == wifiBw) /* for SCO quality at 11b/g mode */
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 2);
	else  /* for SCO quality & wifi performance balance at 11n mode */
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 8);

	halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 0); /* for voice quality */

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, true, 0x4);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, true, 0x4);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, true, 0x4);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, true, 0x4);
		}
	}
}


static void halbtc8723b2ant_ActionHid(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);

	if (BTC_WIFI_BW_LEGACY == wifiBw) /* for HID at 11b/g mode */
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);
	else  /* for HID quality & wifi performance balance at 11n mode */
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 9);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	)
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 9);
	else
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 13);

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

/* A2DP only / PAN(EDR) only/ A2DP+PAN(HS) */
static void halbtc8723b2ant_ActionA2dp(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, wifiRssiState1, btRssiState;
	u32 wifiBw;
	u8 apNum = 0;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	wifiRssiState1 = halbtc8723b2ant_WifiRssiState(pBtCoexist, 1, 2, 40, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U1_AP_NUM, &apNum);

	/*  define the office environment */
	if (apNum >= 10 && BTC_RSSI_HIGH(wifiRssiState1)) {
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
		halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);
		halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 0);
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);

		/*  sw mechanism */
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
		if (BTC_WIFI_BW_HT40 == wifiBw) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, true, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, true, 0x18);
		}
		return;
	}

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	)
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, false, 1);
	else
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, true, 1);

	/*  sw mechanism */
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

static void halbtc8723b2ant_ActionA2dpPanHs(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);

	halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, true, 2);

	/*  sw mechanism */
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

static void halbtc8723b2ant_ActionPanEdr(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 10);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	)
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 1);
	else
		halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, true, 5);

	/*  sw mechanism */
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}


/* PAN(HS) only */
static void halbtc8723b2ant_ActionPanHs(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);

	halbtc8723b2ant_PsTdma(pBtCoexist, NORMAL_EXEC, false, 1);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

/* PAN(EDR)+A2DP */
static void halbtc8723b2ant_ActionPanEdrA2dp(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	) {
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 12);
		if (BTC_WIFI_BW_HT40 == wifiBw)
			halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, true, 3);
		else
			halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, false, 3);
	} else {
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, false, true, 3);
	}

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, false, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

static void halbtc8723b2ant_ActionPanEdrHid(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	) {
		if (BTC_WIFI_BW_HT40 == wifiBw) {
			halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 3);
			halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 11);
			pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x780);
		} else {
			halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);
			halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);
			pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
		}
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, false, 2);
	} else {
		halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);
		halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 11);
		pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, true, 2);
	}

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

/*  HID+A2DP+PAN(EDR) */
static void halbtc8723b2ant_ActionHidA2dpPanEdr(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, false, 0x8);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	if (BTC_RSSI_HIGH(btRssiState))
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
	else
		halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	) {
		if (BTC_WIFI_BW_HT40 == wifiBw)
			halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, true, 2);
		else
			halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, false, 3);
	} else
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, true, 3);

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

static void halbtc8723b2ant_ActionHidA2dp(struct btc_coexist *pBtCoexist)
{
	u8 wifiRssiState, btRssiState;
	u32 wifiBw;
	u8 apNum = 0;

	wifiRssiState = halbtc8723b2ant_WifiRssiState(pBtCoexist, 0, 2, 15, 0);
	/* btRssiState = halbtc8723b2ant_BtRssiState(2, 29, 0); */
	btRssiState = halbtc8723b2ant_BtRssiState(3, 29, 37);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x0);

	halbtc8723b2ant_LimitedRx(pBtCoexist, NORMAL_EXEC, false, true, 0x5);

	halbtc8723b2ant_FwDacSwingLvl(pBtCoexist, NORMAL_EXEC, 6);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
	if (BTC_WIFI_BW_LEGACY == wifiBw) {
		if (BTC_RSSI_HIGH(btRssiState))
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
		else if (BTC_RSSI_MEDIUM(btRssiState))
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
		else
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);
	} else {
		/*  only 802.11N mode we have to dec bt power to 4 degree */
		if (BTC_RSSI_HIGH(btRssiState)) {
			pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U1_AP_NUM, &apNum);
			/*  need to check ap Number of Not */
			if (apNum < 10)
				halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 4);
			else
				halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
		} else if (BTC_RSSI_MEDIUM(btRssiState))
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 2);
		else
			halbtc8723b2ant_DecBtPwr(pBtCoexist, NORMAL_EXEC, 0);
	}

	halbtc8723b2ant_CoexTableWithType(pBtCoexist, NORMAL_EXEC, 7);

	if (
		(btRssiState == BTC_RSSI_STATE_HIGH) ||
		(btRssiState == BTC_RSSI_STATE_STAY_HIGH)
	)
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, false, 2);
	else
		halbtc8723b2ant_TdmaDurationAdjust(pBtCoexist, true, true, 2);

	/*  sw mechanism */
	if (BTC_WIFI_BW_HT40 == wifiBw) {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, true, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	} else {
		if (
			(wifiRssiState == BTC_RSSI_STATE_HIGH) ||
			(wifiRssiState == BTC_RSSI_STATE_STAY_HIGH)
		) {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, true, false, false, 0x18);
		} else {
			halbtc8723b2ant_SwMechanism1(pBtCoexist, false, true, false, false);
			halbtc8723b2ant_SwMechanism2(pBtCoexist, false, false, false, 0x18);
		}
	}
}

static void halbtc8723b2ant_RunCoexistMechanism(struct btc_coexist *pBtCoexist)
{
	u8 algorithm = 0;

	if (pBtCoexist->bManualControl) {
		return;
	}

	if (pCoexSta->bUnderIps) {
		return;
	}

	algorithm = halbtc8723b2ant_ActionAlgorithm(pBtCoexist);
	if (pCoexSta->bC2hBtInquiryPage && (BT_8723B_2ANT_COEX_ALGO_PANHS != algorithm)) {
		halbtc8723b2ant_ActionBtInquiry(pBtCoexist);
		return;
	} else {
		if (pCoexDm->bNeedRecover0x948) {
			pCoexDm->bNeedRecover0x948 = false;
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, pCoexDm->backup0x948);
		}
	}

	pCoexDm->curAlgorithm = algorithm;

	if (halbtc8723b2ant_IsCommonAction(pBtCoexist)) {
		pCoexDm->bAutoTdmaAdjust = false;
	} else {
		if (pCoexDm->curAlgorithm != pCoexDm->preAlgorithm) {
			pCoexDm->bAutoTdmaAdjust = false;
		}


		switch (pCoexDm->curAlgorithm) {
		case BT_8723B_2ANT_COEX_ALGO_SCO:
			halbtc8723b2ant_ActionSco(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_HID:
			halbtc8723b2ant_ActionHid(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_A2DP:
			halbtc8723b2ant_ActionA2dp(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_A2DP_PANHS:
			halbtc8723b2ant_ActionA2dpPanHs(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_PANEDR:
			halbtc8723b2ant_ActionPanEdr(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_PANHS:
			halbtc8723b2ant_ActionPanHs(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_PANEDR_A2DP:
			halbtc8723b2ant_ActionPanEdrA2dp(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_PANEDR_HID:
			halbtc8723b2ant_ActionPanEdrHid(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_HID_A2DP_PANEDR:
			halbtc8723b2ant_ActionHidA2dpPanEdr(pBtCoexist);
			break;
		case BT_8723B_2ANT_COEX_ALGO_HID_A2DP:
			halbtc8723b2ant_ActionHidA2dp(pBtCoexist);
			break;
		default:
			halbtc8723b2ant_CoexAllOff(pBtCoexist);
			break;
		}
		pCoexDm->preAlgorithm = pCoexDm->curAlgorithm;
	}
}

static void halbtc8723b2ant_WifiOffHwCfg(struct btc_coexist *pBtCoexist)
{
	bool bIsInMpMode = false;
	u8 H2C_Parameter[2] = {0};
	u32 fwVer = 0;

	/*  set wlan_act to low */
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x76e, 0x4);

	pBtCoexist->fBtcSetRfReg(pBtCoexist, BTC_RF_A, 0x1, 0xfffff, 0x780); /* WiFi goto standby while GNT_BT 0-->1 */
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_FW_VER, &fwVer);
	if (fwVer >= 0x180000) {
		/* Use H2C to set GNT_BT to HIGH */
		H2C_Parameter[0] = 1;
		pBtCoexist->fBtcFillH2c(pBtCoexist, 0x6E, 1, H2C_Parameter);
	} else
		pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x765, 0x18);

	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_IS_IN_MP_MODE, &bIsInMpMode);
	if (!bIsInMpMode)
		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x67, 0x20, 0x0); /* BT select s0/s1 is controlled by BT */
	else
		pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x67, 0x20, 0x1); /* BT select s0/s1 is controlled by WiFi */
}

static void halbtc8723b2ant_InitHwConfig(struct btc_coexist *pBtCoexist, bool bBackUp)
{
	u8 u1Tmp = 0;

	/*  backup rf 0x1e value */
	pCoexDm->btRf0x1eBackup =
		pBtCoexist->fBtcGetRfReg(pBtCoexist, BTC_RF_A, 0x1e, 0xfffff);

	/*  0x790[5:0]= 0x5 */
	u1Tmp = pBtCoexist->fBtcRead1Byte(pBtCoexist, 0x790);
	u1Tmp &= 0xc0;
	u1Tmp |= 0x5;
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x790, u1Tmp);

	/* Antenna config */
	halbtc8723b2ant_SetAntPath(pBtCoexist, BTC_ANT_WIFI_AT_MAIN, true, false);

	/*  PTA parameter */
	halbtc8723b2ant_CoexTableWithType(pBtCoexist, FORCE_EXEC, 0);

	/*  Enable counter statistics */
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x76e, 0xc); /* 0x76e[3] = 1, WLAN_Act control by PTA */
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x778, 0x3);
	pBtCoexist->fBtcWrite1ByteBitMask(pBtCoexist, 0x40, 0x20, 0x1);
}

/*  */
/*  work around function start with wa_halbtc8723b2ant_ */
/*  */
/*  */
/*  extern function start with EXhalbtc8723b2ant_ */
/*  */
void EXhalbtc8723b2ant_PowerOnSetting(struct btc_coexist *pBtCoexist)
{
	struct btc_board_info *pBoardInfo = &pBtCoexist->boardInfo;
	u8 u1Tmp = 0x4; /* Set BIT2 by default since it's 2ant case */
	u16 u2Tmp = 0x0;

	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x67, 0x20);

	/*  enable BB, REG_SYS_FUNC_EN such that we can write 0x948 correctly. */
	u2Tmp = pBtCoexist->fBtcRead2Byte(pBtCoexist, 0x2);
	pBtCoexist->fBtcWrite2Byte(pBtCoexist, 0x2, u2Tmp | BIT0 | BIT1);

	/*  set GRAN_BT = 1 */
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x765, 0x18);
	/*  set WLAN_ACT = 0 */
	pBtCoexist->fBtcWrite1Byte(pBtCoexist, 0x76e, 0x4);

	/*  */
	/*  S0 or S1 setting and Local register setting(By the setting fw can get ant number, S0/S1, ... info) */
	/*  Local setting bit define */
	/* 	BIT0: "0" for no antenna inverse; "1" for antenna inverse */
	/* 	BIT1: "0" for internal switch; "1" for external switch */
	/* 	BIT2: "0" for one antenna; "1" for two antenna */
	/*  NOTE: here default all internal switch and 1-antenna ==> BIT1 = 0 and BIT2 = 0 */
	if (pBtCoexist->chipInterface == BTC_INTF_USB) {
		/*  fixed at S0 for USB interface */
		pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x0);

		u1Tmp |= 0x1;	/*  antenna inverse */
		pBtCoexist->fBtcWriteLocalReg1Byte(pBtCoexist, 0xfe08, u1Tmp);

		pBoardInfo->btdmAntPos = BTC_ANTENNA_AT_AUX_PORT;
	} else {
		/*  for PCIE and SDIO interface, we check efuse 0xc3[6] */
		if (pBoardInfo->singleAntPath == 0) {
			/*  set to S1 */
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x280);
			pBoardInfo->btdmAntPos = BTC_ANTENNA_AT_MAIN_PORT;
		} else if (pBoardInfo->singleAntPath == 1) {
			/*  set to S0 */
			pBtCoexist->fBtcWrite4Byte(pBtCoexist, 0x948, 0x0);
			u1Tmp |= 0x1;	/*  antenna inverse */
			pBoardInfo->btdmAntPos = BTC_ANTENNA_AT_AUX_PORT;
		}

		if (pBtCoexist->chipInterface == BTC_INTF_PCI)
			pBtCoexist->fBtcWriteLocalReg1Byte(pBtCoexist, 0x384, u1Tmp);
		else if (pBtCoexist->chipInterface == BTC_INTF_SDIO)
			pBtCoexist->fBtcWriteLocalReg1Byte(pBtCoexist, 0x60, u1Tmp);
	}
}

void EXhalbtc8723b2ant_InitHwConfig(struct btc_coexist *pBtCoexist, bool bWifiOnly)
{
	halbtc8723b2ant_InitHwConfig(pBtCoexist, true);
}

void EXhalbtc8723b2ant_InitCoexDm(struct btc_coexist *pBtCoexist)
{
	halbtc8723b2ant_InitCoexDm(pBtCoexist);
}

void EXhalbtc8723b2ant_IpsNotify(struct btc_coexist *pBtCoexist, u8 type)
{
	if (BTC_IPS_ENTER == type) {
		pCoexSta->bUnderIps = true;
		halbtc8723b2ant_WifiOffHwCfg(pBtCoexist);
		halbtc8723b2ant_IgnoreWlanAct(pBtCoexist, FORCE_EXEC, true);
		halbtc8723b2ant_CoexAllOff(pBtCoexist);
	} else if (BTC_IPS_LEAVE == type) {
		pCoexSta->bUnderIps = false;
		halbtc8723b2ant_InitHwConfig(pBtCoexist, false);
		halbtc8723b2ant_InitCoexDm(pBtCoexist);
		halbtc8723b2ant_QueryBtInfo(pBtCoexist);
	}
}

void EXhalbtc8723b2ant_LpsNotify(struct btc_coexist *pBtCoexist, u8 type)
{
	if (BTC_LPS_ENABLE == type) {
		pCoexSta->bUnderLps = true;
	} else if (BTC_LPS_DISABLE == type) {
		pCoexSta->bUnderLps = false;
	}
}

void EXhalbtc8723b2ant_ScanNotify(struct btc_coexist *pBtCoexist, u8 type)
{
	if (BTC_SCAN_START == type) {
	} else if (BTC_SCAN_FINISH == type) {
	}
}

void EXhalbtc8723b2ant_ConnectNotify(struct btc_coexist *pBtCoexist, u8 type)
{
	if (BTC_ASSOCIATE_START == type) {
	} else if (BTC_ASSOCIATE_FINISH == type) {
	}
}

void EXhalbtc8723b2ant_MediaStatusNotify(struct btc_coexist *pBtCoexist, u8 type)
{
	u8 H2C_Parameter[3] = {0};
	u32 wifiBw;
	u8 wifiCentralChnl;
	u8 apNum = 0;

	/*  only 2.4G we need to inform bt the chnl mask */
	pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U1_WIFI_CENTRAL_CHNL, &wifiCentralChnl);
	if ((BTC_MEDIA_CONNECT == type) && (wifiCentralChnl <= 14)) {
		H2C_Parameter[0] = 0x1;
		H2C_Parameter[1] = wifiCentralChnl;
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_BW, &wifiBw);
		if (BTC_WIFI_BW_HT40 == wifiBw)
			H2C_Parameter[2] = 0x30;
		else {
			pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U1_AP_NUM, &apNum);
			if (apNum < 10)
				H2C_Parameter[2] = 0x30;
			else
				H2C_Parameter[2] = 0x20;
		}
	}

	pCoexDm->wifiChnlInfo[0] = H2C_Parameter[0];
	pCoexDm->wifiChnlInfo[1] = H2C_Parameter[1];
	pCoexDm->wifiChnlInfo[2] = H2C_Parameter[2];

	pBtCoexist->fBtcFillH2c(pBtCoexist, 0x66, 3, H2C_Parameter);
}

void EXhalbtc8723b2ant_SpecialPacketNotify(struct btc_coexist *pBtCoexist, u8 type)
{
}

void EXhalbtc8723b2ant_BtInfoNotify(
	struct btc_coexist *pBtCoexist, u8 *tmpBuf, u8 length
)
{
	u8 	btInfo = 0;
	u8 	i, rspSource = 0;
	bool bBtBusy = false, bLimitedDig = false;
	bool bWifiConnected = false;

	pCoexSta->bC2hBtInfoReqSent = false;

	rspSource = tmpBuf[0] & 0xf;
	if (rspSource >= BT_INFO_SRC_8723B_2ANT_MAX)
		rspSource = BT_INFO_SRC_8723B_2ANT_WIFI_FW;

	pCoexSta->btInfoC2hCnt[rspSource]++;

	for (i = 0; i < length; i++) {
		pCoexSta->btInfoC2h[rspSource][i] = tmpBuf[i];
		if (i == 1)
			btInfo = tmpBuf[i];

	}

	if (pBtCoexist->bManualControl) {
		return;
	}

	if (BT_INFO_SRC_8723B_2ANT_WIFI_FW != rspSource) {
		pCoexSta->btRetryCnt = pCoexSta->btInfoC2h[rspSource][2] & 0xf; /* [3:0] */

		pCoexSta->btRssi = pCoexSta->btInfoC2h[rspSource][3] * 2 + 10;

		pCoexSta->btInfoExt = pCoexSta->btInfoC2h[rspSource][4];

		pCoexSta->bBtTxRxMask = (pCoexSta->btInfoC2h[rspSource][2] & 0x40);
		pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_BL_BT_TX_RX_MASK, &pCoexSta->bBtTxRxMask);
		if (pCoexSta->bBtTxRxMask) {
			/* BT into is responded by BT FW and BT RF REG 0x3C != 0x01 => Need to switch BT TRx Mask */
			pBtCoexist->fBtcSetBtReg(pBtCoexist, BTC_BT_REG_RF, 0x3c, 0x01);
		}

		/*  Here we need to resend some wifi info to BT */
		/*  because bt is reset and loss of the info. */
		if ((pCoexSta->btInfoExt & BIT1)) {
			pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_BL_WIFI_CONNECTED, &bWifiConnected);

			if (bWifiConnected)
				EXhalbtc8723b2ant_MediaStatusNotify(pBtCoexist, BTC_MEDIA_CONNECT);
			else
				EXhalbtc8723b2ant_MediaStatusNotify(pBtCoexist, BTC_MEDIA_DISCONNECT);
		}

		if ((pCoexSta->btInfoExt & BIT3)) {
			halbtc8723b2ant_IgnoreWlanAct(pBtCoexist, FORCE_EXEC, false);
		} else {
			/*  BT already NOT ignore Wlan active, do nothing here. */
		}
	}

	/*  check BIT2 first ==> check if bt is under inquiry or page scan */
	if (btInfo & BT_INFO_8723B_2ANT_B_INQ_PAGE)
		pCoexSta->bC2hBtInquiryPage = true;
	else
		pCoexSta->bC2hBtInquiryPage = false;

	/*  set link exist status */
	if (!(btInfo & BT_INFO_8723B_2ANT_B_CONNECTION)) {
		pCoexSta->bBtLinkExist = false;
		pCoexSta->bPanExist = false;
		pCoexSta->bA2dpExist = false;
		pCoexSta->bHidExist = false;
		pCoexSta->bScoExist = false;
	} else { /*  connection exists */
		pCoexSta->bBtLinkExist = true;
		if (btInfo & BT_INFO_8723B_2ANT_B_FTP)
			pCoexSta->bPanExist = true;
		else
			pCoexSta->bPanExist = false;
		if (btInfo & BT_INFO_8723B_2ANT_B_A2DP)
			pCoexSta->bA2dpExist = true;
		else
			pCoexSta->bA2dpExist = false;
		if (btInfo & BT_INFO_8723B_2ANT_B_HID)
			pCoexSta->bHidExist = true;
		else
			pCoexSta->bHidExist = false;
		if (btInfo & BT_INFO_8723B_2ANT_B_SCO_ESCO)
			pCoexSta->bScoExist = true;
		else
			pCoexSta->bScoExist = false;
	}

	halbtc8723b2ant_UpdateBtLinkInfo(pBtCoexist);

	if (!(btInfo & BT_INFO_8723B_2ANT_B_CONNECTION)) {
		pCoexDm->btStatus = BT_8723B_2ANT_BT_STATUS_NON_CONNECTED_IDLE;
	} else if (btInfo == BT_INFO_8723B_2ANT_B_CONNECTION)	{ /*  connection exists but no busy */
		pCoexDm->btStatus = BT_8723B_2ANT_BT_STATUS_CONNECTED_IDLE;
	} else if (
		(btInfo & BT_INFO_8723B_2ANT_B_SCO_ESCO) ||
		(btInfo & BT_INFO_8723B_2ANT_B_SCO_BUSY)
	) {
		pCoexDm->btStatus = BT_8723B_2ANT_BT_STATUS_SCO_BUSY;
	} else if (btInfo & BT_INFO_8723B_2ANT_B_ACL_BUSY) {
		pCoexDm->btStatus = BT_8723B_2ANT_BT_STATUS_ACL_BUSY;
	} else {
		pCoexDm->btStatus = BT_8723B_2ANT_BT_STATUS_MAX;
	}

	if (
		(BT_8723B_2ANT_BT_STATUS_ACL_BUSY == pCoexDm->btStatus) ||
		(BT_8723B_2ANT_BT_STATUS_SCO_BUSY == pCoexDm->btStatus) ||
		(BT_8723B_2ANT_BT_STATUS_ACL_SCO_BUSY == pCoexDm->btStatus)
	) {
		bBtBusy = true;
		bLimitedDig = true;
	} else {
		bBtBusy = false;
		bLimitedDig = false;
	}

	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_BL_BT_TRAFFIC_BUSY, &bBtBusy);

	pCoexDm->bLimitedDig = bLimitedDig;
	pBtCoexist->fBtcSet(pBtCoexist, BTC_SET_BL_BT_LIMITED_DIG, &bLimitedDig);

	halbtc8723b2ant_RunCoexistMechanism(pBtCoexist);
}

void EXhalbtc8723b2ant_HaltNotify(struct btc_coexist *pBtCoexist)
{
	halbtc8723b2ant_WifiOffHwCfg(pBtCoexist);
	pBtCoexist->fBtcSetBtReg(pBtCoexist, BTC_BT_REG_RF, 0x3c, 0x15); /* BT goto standby while GNT_BT 1-->0 */
	halbtc8723b2ant_IgnoreWlanAct(pBtCoexist, FORCE_EXEC, true);

	EXhalbtc8723b2ant_MediaStatusNotify(pBtCoexist, BTC_MEDIA_DISCONNECT);
}

void EXhalbtc8723b2ant_PnpNotify(struct btc_coexist *pBtCoexist, u8 pnpState)
{
	if (BTC_WIFI_PNP_SLEEP == pnpState) {
	} else if (BTC_WIFI_PNP_WAKE_UP == pnpState) {
		halbtc8723b2ant_InitHwConfig(pBtCoexist, false);
		halbtc8723b2ant_InitCoexDm(pBtCoexist);
		halbtc8723b2ant_QueryBtInfo(pBtCoexist);
	}
}

void EXhalbtc8723b2ant_Periodical(struct btc_coexist *pBtCoexist)
{
	static u8 disVerInfoCnt;
	u32 fwVer = 0, btPatchVer = 0;

	if (disVerInfoCnt <= 5) {
		disVerInfoCnt += 1;
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_BT_PATCH_VER, &btPatchVer);
		pBtCoexist->fBtcGet(pBtCoexist, BTC_GET_U4_WIFI_FW_VER, &fwVer);
	}

	if (
		halbtc8723b2ant_IsWifiStatusChanged(pBtCoexist) ||
		pCoexDm->bAutoTdmaAdjust
	)
		halbtc8723b2ant_RunCoexistMechanism(pBtCoexist);
}
