// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021, Intel Corporation. */

#include "ice.h"
#include "ice_lib.h"
#include "ice_trace.h"

#define E810_OUT_PROP_DELAY_NS 1

#define UNKNOWN_INCVAL_E822 0x100000000ULL

static const struct ptp_pin_desc ice_pin_desc_e810t[] = {
	/* name    idx   func         chan */
	{ "GNSS",  GNSS, PTP_PF_EXTTS, 0, { 0, } },
	{ "SMA1",  SMA1, PTP_PF_NONE, 1, { 0, } },
	{ "U.FL1", UFL1, PTP_PF_NONE, 1, { 0, } },
	{ "SMA2",  SMA2, PTP_PF_NONE, 2, { 0, } },
	{ "U.FL2", UFL2, PTP_PF_NONE, 2, { 0, } },
};

/**
 * ice_get_sma_config_e810t
 * @hw: pointer to the hw struct
 * @ptp_pins: pointer to the ptp_pin_desc struture
 *
 * Read the configuration of the SMA control logic and put it into the
 * ptp_pin_desc structure
 */
static int
ice_get_sma_config_e810t(struct ice_hw *hw, struct ptp_pin_desc *ptp_pins)
{
	u8 data, i;
	int status;

	/* Read initial pin state */
	status = ice_read_sma_ctrl_e810t(hw, &data);
	if (status)
		return status;

	/* initialize with defaults */
	for (i = 0; i < NUM_PTP_PINS_E810T; i++) {
		snprintf(ptp_pins[i].name, sizeof(ptp_pins[i].name),
			 "%s", ice_pin_desc_e810t[i].name);
		ptp_pins[i].index = ice_pin_desc_e810t[i].index;
		ptp_pins[i].func = ice_pin_desc_e810t[i].func;
		ptp_pins[i].chan = ice_pin_desc_e810t[i].chan;
	}

	/* Parse SMA1/UFL1 */
	switch (data & ICE_SMA1_MASK_E810T) {
	case ICE_SMA1_MASK_E810T:
	default:
		ptp_pins[SMA1].func = PTP_PF_NONE;
		ptp_pins[UFL1].func = PTP_PF_NONE;
		break;
	case ICE_SMA1_DIR_EN_E810T:
		ptp_pins[SMA1].func = PTP_PF_PEROUT;
		ptp_pins[UFL1].func = PTP_PF_NONE;
		break;
	case ICE_SMA1_TX_EN_E810T:
		ptp_pins[SMA1].func = PTP_PF_EXTTS;
		ptp_pins[UFL1].func = PTP_PF_NONE;
		break;
	case 0:
		ptp_pins[SMA1].func = PTP_PF_EXTTS;
		ptp_pins[UFL1].func = PTP_PF_PEROUT;
		break;
	}

	/* Parse SMA2/UFL2 */
	switch (data & ICE_SMA2_MASK_E810T) {
	case ICE_SMA2_MASK_E810T:
	default:
		ptp_pins[SMA2].func = PTP_PF_NONE;
		ptp_pins[UFL2].func = PTP_PF_NONE;
		break;
	case (ICE_SMA2_TX_EN_E810T | ICE_SMA2_UFL2_RX_DIS_E810T):
		ptp_pins[SMA2].func = PTP_PF_EXTTS;
		ptp_pins[UFL2].func = PTP_PF_NONE;
		break;
	case (ICE_SMA2_DIR_EN_E810T | ICE_SMA2_UFL2_RX_DIS_E810T):
		ptp_pins[SMA2].func = PTP_PF_PEROUT;
		ptp_pins[UFL2].func = PTP_PF_NONE;
		break;
	case (ICE_SMA2_DIR_EN_E810T | ICE_SMA2_TX_EN_E810T):
		ptp_pins[SMA2].func = PTP_PF_NONE;
		ptp_pins[UFL2].func = PTP_PF_EXTTS;
		break;
	case ICE_SMA2_DIR_EN_E810T:
		ptp_pins[SMA2].func = PTP_PF_PEROUT;
		ptp_pins[UFL2].func = PTP_PF_EXTTS;
		break;
	}

	return 0;
}

/**
 * ice_ptp_set_sma_config_e810t
 * @hw: pointer to the hw struct
 * @ptp_pins: pointer to the ptp_pin_desc struture
 *
 * Set the configuration of the SMA control logic based on the configuration in
 * num_pins parameter
 */
static int
ice_ptp_set_sma_config_e810t(struct ice_hw *hw,
			     const struct ptp_pin_desc *ptp_pins)
{
	int status;
	u8 data;

	/* SMA1 and UFL1 cannot be set to TX at the same time */
	if (ptp_pins[SMA1].func == PTP_PF_PEROUT &&
	    ptp_pins[UFL1].func == PTP_PF_PEROUT)
		return -EINVAL;

	/* SMA2 and UFL2 cannot be set to RX at the same time */
	if (ptp_pins[SMA2].func == PTP_PF_EXTTS &&
	    ptp_pins[UFL2].func == PTP_PF_EXTTS)
		return -EINVAL;

	/* Read initial pin state value */
	status = ice_read_sma_ctrl_e810t(hw, &data);
	if (status)
		return status;

	/* Set the right sate based on the desired configuration */
	data &= ~ICE_SMA1_MASK_E810T;
	if (ptp_pins[SMA1].func == PTP_PF_NONE &&
	    ptp_pins[UFL1].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA1 + U.FL1 disabled");
		data |= ICE_SMA1_MASK_E810T;
	} else if (ptp_pins[SMA1].func == PTP_PF_EXTTS &&
		   ptp_pins[UFL1].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA1 RX");
		data |= ICE_SMA1_TX_EN_E810T;
	} else if (ptp_pins[SMA1].func == PTP_PF_NONE &&
		   ptp_pins[UFL1].func == PTP_PF_PEROUT) {
		/* U.FL 1 TX will always enable SMA 1 RX */
		dev_info(ice_hw_to_dev(hw), "SMA1 RX + U.FL1 TX");
	} else if (ptp_pins[SMA1].func == PTP_PF_EXTTS &&
		   ptp_pins[UFL1].func == PTP_PF_PEROUT) {
		dev_info(ice_hw_to_dev(hw), "SMA1 RX + U.FL1 TX");
	} else if (ptp_pins[SMA1].func == PTP_PF_PEROUT &&
		   ptp_pins[UFL1].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA1 TX");
		data |= ICE_SMA1_DIR_EN_E810T;
	}

	data &= ~ICE_SMA2_MASK_E810T;
	if (ptp_pins[SMA2].func == PTP_PF_NONE &&
	    ptp_pins[UFL2].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA2 + U.FL2 disabled");
		data |= ICE_SMA2_MASK_E810T;
	} else if (ptp_pins[SMA2].func == PTP_PF_EXTTS &&
			ptp_pins[UFL2].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA2 RX");
		data |= (ICE_SMA2_TX_EN_E810T |
			 ICE_SMA2_UFL2_RX_DIS_E810T);
	} else if (ptp_pins[SMA2].func == PTP_PF_NONE &&
		   ptp_pins[UFL2].func == PTP_PF_EXTTS) {
		dev_info(ice_hw_to_dev(hw), "UFL2 RX");
		data |= (ICE_SMA2_DIR_EN_E810T | ICE_SMA2_TX_EN_E810T);
	} else if (ptp_pins[SMA2].func == PTP_PF_PEROUT &&
		   ptp_pins[UFL2].func == PTP_PF_NONE) {
		dev_info(ice_hw_to_dev(hw), "SMA2 TX");
		data |= (ICE_SMA2_DIR_EN_E810T |
			 ICE_SMA2_UFL2_RX_DIS_E810T);
	} else if (ptp_pins[SMA2].func == PTP_PF_PEROUT &&
		   ptp_pins[UFL2].func == PTP_PF_EXTTS) {
		dev_info(ice_hw_to_dev(hw), "SMA2 TX + U.FL2 RX");
		data |= ICE_SMA2_DIR_EN_E810T;
	}

	return ice_write_sma_ctrl_e810t(hw, data);
}

/**
 * ice_ptp_set_sma_e810t
 * @info: the driver's PTP info structure
 * @pin: pin index in kernel structure
 * @func: Pin function to be set (PTP_PF_NONE, PTP_PF_EXTTS or PTP_PF_PEROUT)
 *
 * Set the configuration of a single SMA pin
 */
static int
ice_ptp_set_sma_e810t(struct ptp_clock_info *info, unsigned int pin,
		      enum ptp_pin_function func)
{
	struct ptp_pin_desc ptp_pins[NUM_PTP_PINS_E810T];
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_hw *hw = &pf->hw;
	int err;

	if (pin < SMA1 || func > PTP_PF_PEROUT)
		return -EOPNOTSUPP;

	err = ice_get_sma_config_e810t(hw, ptp_pins);
	if (err)
		return err;

	/* Disable the same function on the other pin sharing the channel */
	if (pin == SMA1 && ptp_pins[UFL1].func == func)
		ptp_pins[UFL1].func = PTP_PF_NONE;
	if (pin == UFL1 && ptp_pins[SMA1].func == func)
		ptp_pins[SMA1].func = PTP_PF_NONE;

	if (pin == SMA2 && ptp_pins[UFL2].func == func)
		ptp_pins[UFL2].func = PTP_PF_NONE;
	if (pin == UFL2 && ptp_pins[SMA2].func == func)
		ptp_pins[SMA2].func = PTP_PF_NONE;

	/* Set up new pin function in the temp table */
	ptp_pins[pin].func = func;

	return ice_ptp_set_sma_config_e810t(hw, ptp_pins);
}

/**
 * ice_verify_pin_e810t
 * @info: the driver's PTP info structure
 * @pin: Pin index
 * @func: Assigned function
 * @chan: Assigned channel
 *
 * Verify if pin supports requested pin function. If the Check pins consistency.
 * Reconfigure the SMA logic attached to the given pin to enable its
 * desired functionality
 */
static int
ice_verify_pin_e810t(struct ptp_clock_info *info, unsigned int pin,
		     enum ptp_pin_function func, unsigned int chan)
{
	/* Don't allow channel reassignment */
	if (chan != ice_pin_desc_e810t[pin].chan)
		return -EOPNOTSUPP;

	/* Check if functions are properly assigned */
	switch (func) {
	case PTP_PF_NONE:
		break;
	case PTP_PF_EXTTS:
		if (pin == UFL1)
			return -EOPNOTSUPP;
		break;
	case PTP_PF_PEROUT:
		if (pin == UFL2 || pin == GNSS)
			return -EOPNOTSUPP;
		break;
	case PTP_PF_PHYSYNC:
		return -EOPNOTSUPP;
	}

	return ice_ptp_set_sma_e810t(info, pin, func);
}

/**
 * ice_set_tx_tstamp - Enable or disable Tx timestamping
 * @pf: The PF pointer to search in
 * @on: bool value for whether timestamps are enabled or disabled
 */
static void ice_set_tx_tstamp(struct ice_pf *pf, bool on)
{
	struct ice_vsi *vsi;
	u32 val;
	u16 i;

	vsi = ice_get_main_vsi(pf);
	if (!vsi)
		return;

	/* Set the timestamp enable flag for all the Tx rings */
	ice_for_each_txq(vsi, i) {
		if (!vsi->tx_rings[i])
			continue;
		vsi->tx_rings[i]->ptp_tx = on;
	}

	/* Configure the Tx timestamp interrupt */
	val = rd32(&pf->hw, PFINT_OICR_ENA);
	if (on)
		val |= PFINT_OICR_TSYN_TX_M;
	else
		val &= ~PFINT_OICR_TSYN_TX_M;
	wr32(&pf->hw, PFINT_OICR_ENA, val);

	pf->ptp.tstamp_config.tx_type = on ? HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF;
}

/**
 * ice_set_rx_tstamp - Enable or disable Rx timestamping
 * @pf: The PF pointer to search in
 * @on: bool value for whether timestamps are enabled or disabled
 */
static void ice_set_rx_tstamp(struct ice_pf *pf, bool on)
{
	struct ice_vsi *vsi;
	u16 i;

	vsi = ice_get_main_vsi(pf);
	if (!vsi)
		return;

	/* Set the timestamp flag for all the Rx rings */
	ice_for_each_rxq(vsi, i) {
		if (!vsi->rx_rings[i])
			continue;
		vsi->rx_rings[i]->ptp_rx = on;
	}

	pf->ptp.tstamp_config.rx_filter = on ? HWTSTAMP_FILTER_ALL :
					       HWTSTAMP_FILTER_NONE;
}

/**
 * ice_ptp_cfg_timestamp - Configure timestamp for init/deinit
 * @pf: Board private structure
 * @ena: bool value to enable or disable time stamp
 *
 * This function will configure timestamping during PTP initialization
 * and deinitialization
 */
void ice_ptp_cfg_timestamp(struct ice_pf *pf, bool ena)
{
	ice_set_tx_tstamp(pf, ena);
	ice_set_rx_tstamp(pf, ena);
}

/**
 * ice_get_ptp_clock_index - Get the PTP clock index
 * @pf: the PF pointer
 *
 * Determine the clock index of the PTP clock associated with this device. If
 * this is the PF controlling the clock, just use the local access to the
 * clock device pointer.
 *
 * Otherwise, read from the driver shared parameters to determine the clock
 * index value.
 *
 * Returns: the index of the PTP clock associated with this device, or -1 if
 * there is no associated clock.
 */
int ice_get_ptp_clock_index(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	enum ice_aqc_driver_params param_idx;
	struct ice_hw *hw = &pf->hw;
	u8 tmr_idx;
	u32 value;
	int err;

	/* Use the ptp_clock structure if we're the main PF */
	if (pf->ptp.clock)
		return ptp_clock_index(pf->ptp.clock);

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_assoc;
	if (!tmr_idx)
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR0;
	else
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR1;

	err = ice_aq_get_driver_param(hw, param_idx, &value, NULL);
	if (err) {
		dev_err(dev, "Failed to read PTP clock index parameter, err %d aq_err %s\n",
			err, ice_aq_str(hw->adminq.sq_last_status));
		return -1;
	}

	/* The PTP clock index is an integer, and will be between 0 and
	 * INT_MAX. The highest bit of the driver shared parameter is used to
	 * indicate whether or not the currently stored clock index is valid.
	 */
	if (!(value & PTP_SHARED_CLK_IDX_VALID))
		return -1;

	return value & ~PTP_SHARED_CLK_IDX_VALID;
}

/**
 * ice_set_ptp_clock_index - Set the PTP clock index
 * @pf: the PF pointer
 *
 * Set the PTP clock index for this device into the shared driver parameters,
 * so that other PFs associated with this device can read it.
 *
 * If the PF is unable to store the clock index, it will log an error, but
 * will continue operating PTP.
 */
static void ice_set_ptp_clock_index(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	enum ice_aqc_driver_params param_idx;
	struct ice_hw *hw = &pf->hw;
	u8 tmr_idx;
	u32 value;
	int err;

	if (!pf->ptp.clock)
		return;

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_assoc;
	if (!tmr_idx)
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR0;
	else
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR1;

	value = (u32)ptp_clock_index(pf->ptp.clock);
	if (value > INT_MAX) {
		dev_err(dev, "PTP Clock index is too large to store\n");
		return;
	}
	value |= PTP_SHARED_CLK_IDX_VALID;

	err = ice_aq_set_driver_param(hw, param_idx, value, NULL);
	if (err) {
		dev_err(dev, "Failed to set PTP clock index parameter, err %d aq_err %s\n",
			err, ice_aq_str(hw->adminq.sq_last_status));
	}
}

/**
 * ice_clear_ptp_clock_index - Clear the PTP clock index
 * @pf: the PF pointer
 *
 * Clear the PTP clock index for this device. Must be called when
 * unregistering the PTP clock, in order to ensure other PFs stop reporting
 * a clock object that no longer exists.
 */
static void ice_clear_ptp_clock_index(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	enum ice_aqc_driver_params param_idx;
	struct ice_hw *hw = &pf->hw;
	u8 tmr_idx;
	int err;

	/* Do not clear the index if we don't own the timer */
	if (!hw->func_caps.ts_func_info.src_tmr_owned)
		return;

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_assoc;
	if (!tmr_idx)
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR0;
	else
		param_idx = ICE_AQC_DRIVER_PARAM_CLK_IDX_TMR1;

	err = ice_aq_set_driver_param(hw, param_idx, 0, NULL);
	if (err) {
		dev_dbg(dev, "Failed to clear PTP clock index parameter, err %d aq_err %s\n",
			err, ice_aq_str(hw->adminq.sq_last_status));
	}
}

/**
 * ice_ptp_read_src_clk_reg - Read the source clock register
 * @pf: Board private structure
 * @sts: Optional parameter for holding a pair of system timestamps from
 *       the system clock. Will be ignored if NULL is given.
 */
static u64
ice_ptp_read_src_clk_reg(struct ice_pf *pf, struct ptp_system_timestamp *sts)
{
	struct ice_hw *hw = &pf->hw;
	u32 hi, lo, lo2;
	u8 tmr_idx;

	tmr_idx = ice_get_ptp_src_clock_index(hw);
	/* Read the system timestamp pre PHC read */
	ptp_read_system_prets(sts);

	lo = rd32(hw, GLTSYN_TIME_L(tmr_idx));

	/* Read the system timestamp post PHC read */
	ptp_read_system_postts(sts);

	hi = rd32(hw, GLTSYN_TIME_H(tmr_idx));
	lo2 = rd32(hw, GLTSYN_TIME_L(tmr_idx));

	if (lo2 < lo) {
		/* if TIME_L rolled over read TIME_L again and update
		 * system timestamps
		 */
		ptp_read_system_prets(sts);
		lo = rd32(hw, GLTSYN_TIME_L(tmr_idx));
		ptp_read_system_postts(sts);
		hi = rd32(hw, GLTSYN_TIME_H(tmr_idx));
	}

	return ((u64)hi << 32) | lo;
}

/**
 * ice_ptp_extend_32b_ts - Convert a 32b nanoseconds timestamp to 64b
 * @cached_phc_time: recently cached copy of PHC time
 * @in_tstamp: Ingress/egress 32b nanoseconds timestamp value
 *
 * Hardware captures timestamps which contain only 32 bits of nominal
 * nanoseconds, as opposed to the 64bit timestamps that the stack expects.
 * Note that the captured timestamp values may be 40 bits, but the lower
 * 8 bits are sub-nanoseconds and generally discarded.
 *
 * Extend the 32bit nanosecond timestamp using the following algorithm and
 * assumptions:
 *
 * 1) have a recently cached copy of the PHC time
 * 2) assume that the in_tstamp was captured 2^31 nanoseconds (~2.1
 *    seconds) before or after the PHC time was captured.
 * 3) calculate the delta between the cached time and the timestamp
 * 4) if the delta is smaller than 2^31 nanoseconds, then the timestamp was
 *    captured after the PHC time. In this case, the full timestamp is just
 *    the cached PHC time plus the delta.
 * 5) otherwise, if the delta is larger than 2^31 nanoseconds, then the
 *    timestamp was captured *before* the PHC time, i.e. because the PHC
 *    cache was updated after the timestamp was captured by hardware. In this
 *    case, the full timestamp is the cached time minus the inverse delta.
 *
 * This algorithm works even if the PHC time was updated after a Tx timestamp
 * was requested, but before the Tx timestamp event was reported from
 * hardware.
 *
 * This calculation primarily relies on keeping the cached PHC time up to
 * date. If the timestamp was captured more than 2^31 nanoseconds after the
 * PHC time, it is possible that the lower 32bits of PHC time have
 * overflowed more than once, and we might generate an incorrect timestamp.
 *
 * This is prevented by (a) periodically updating the cached PHC time once
 * a second, and (b) discarding any Tx timestamp packet if it has waited for
 * a timestamp for more than one second.
 */
static u64 ice_ptp_extend_32b_ts(u64 cached_phc_time, u32 in_tstamp)
{
	u32 delta, phc_time_lo;
	u64 ns;

	/* Extract the lower 32 bits of the PHC time */
	phc_time_lo = (u32)cached_phc_time;

	/* Calculate the delta between the lower 32bits of the cached PHC
	 * time and the in_tstamp value
	 */
	delta = (in_tstamp - phc_time_lo);

	/* Do not assume that the in_tstamp is always more recent than the
	 * cached PHC time. If the delta is large, it indicates that the
	 * in_tstamp was taken in the past, and should be converted
	 * forward.
	 */
	if (delta > (U32_MAX / 2)) {
		/* reverse the delta calculation here */
		delta = (phc_time_lo - in_tstamp);
		ns = cached_phc_time - delta;
	} else {
		ns = cached_phc_time + delta;
	}

	return ns;
}

/**
 * ice_ptp_extend_40b_ts - Convert a 40b timestamp to 64b nanoseconds
 * @pf: Board private structure
 * @in_tstamp: Ingress/egress 40b timestamp value
 *
 * The Tx and Rx timestamps are 40 bits wide, including 32 bits of nominal
 * nanoseconds, 7 bits of sub-nanoseconds, and a valid bit.
 *
 *  *--------------------------------------------------------------*
 *  | 32 bits of nanoseconds | 7 high bits of sub ns underflow | v |
 *  *--------------------------------------------------------------*
 *
 * The low bit is an indicator of whether the timestamp is valid. The next
 * 7 bits are a capture of the upper 7 bits of the sub-nanosecond underflow,
 * and the remaining 32 bits are the lower 32 bits of the PHC timer.
 *
 * It is assumed that the caller verifies the timestamp is valid prior to
 * calling this function.
 *
 * Extract the 32bit nominal nanoseconds and extend them. Use the cached PHC
 * time stored in the device private PTP structure as the basis for timestamp
 * extension.
 *
 * See ice_ptp_extend_32b_ts for a detailed explanation of the extension
 * algorithm.
 */
static u64 ice_ptp_extend_40b_ts(struct ice_pf *pf, u64 in_tstamp)
{
	const u64 mask = GENMASK_ULL(31, 0);
	unsigned long discard_time;

	/* Discard the hardware timestamp if the cached PHC time is too old */
	discard_time = pf->ptp.cached_phc_jiffies + msecs_to_jiffies(2000);
	if (time_is_before_jiffies(discard_time)) {
		pf->ptp.tx_hwtstamp_discarded++;
		return 0;
	}

	return ice_ptp_extend_32b_ts(pf->ptp.cached_phc_time,
				     (in_tstamp >> 8) & mask);
}

/**
 * ice_ptp_is_tx_tracker_up - Check if Tx tracker is ready for new timestamps
 * @tx: the PTP Tx timestamp tracker to check
 *
 * Check that a given PTP Tx timestamp tracker is up, i.e. that it is ready
 * to accept new timestamp requests.
 *
 * Assumes the tx->lock spinlock is already held.
 */
static bool
ice_ptp_is_tx_tracker_up(struct ice_ptp_tx *tx)
{
	lockdep_assert_held(&tx->lock);

	return tx->init && !tx->calibrating;
}

/**
 * ice_ptp_process_tx_tstamp - Process Tx timestamps for a port
 * @tx: the PTP Tx timestamp tracker
 *
 * Process timestamps captured by the PHY associated with this port. To do
 * this, loop over each index with a waiting skb.
 *
 * If a given index has a valid timestamp, perform the following steps:
 *
 * 1) check that the timestamp request is not stale
 * 2) check that a timestamp is ready and available in the PHY memory bank
 * 3) read and copy the timestamp out of the PHY register
 * 4) unlock the index by clearing the associated in_use bit
 * 5) check if the timestamp is stale, and discard if so
 * 6) extend the 40 bit timestamp value to get a 64 bit timestamp value
 * 7) send this 64 bit timestamp to the stack
 *
 * Note that we do not hold the tracking lock while reading the Tx timestamp.
 * This is because reading the timestamp requires taking a mutex that might
 * sleep.
 *
 * The only place where we set in_use is when a new timestamp is initiated
 * with a slot index. This is only called in the hard xmit routine where an
 * SKB has a request flag set. The only places where we clear this bit is this
 * function, or during teardown when the Tx timestamp tracker is being
 * removed. A timestamp index will never be re-used until the in_use bit for
 * that index is cleared.
 *
 * If a Tx thread starts a new timestamp, we might not begin processing it
 * right away but we will notice it at the end when we re-queue the task.
 *
 * If a Tx thread starts a new timestamp just after this function exits, the
 * interrupt for that timestamp should re-trigger this function once
 * a timestamp is ready.
 *
 * In cases where the PTP hardware clock was directly adjusted, some
 * timestamps may not be able to safely use the timestamp extension math. In
 * this case, software will set the stale bit for any outstanding Tx
 * timestamps when the clock is adjusted. Then this function will discard
 * those captured timestamps instead of sending them to the stack.
 *
 * If a Tx packet has been waiting for more than 2 seconds, it is not possible
 * to correctly extend the timestamp using the cached PHC time. It is
 * extremely unlikely that a packet will ever take this long to timestamp. If
 * we detect a Tx timestamp request that has waited for this long we assume
 * the packet will never be sent by hardware and discard it without reading
 * the timestamp register.
 */
static void ice_ptp_process_tx_tstamp(struct ice_ptp_tx *tx)
{
	struct ice_ptp_port *ptp_port;
	struct ice_pf *pf;
	struct ice_hw *hw;
	u64 tstamp_ready;
	bool link_up;
	int err;
	u8 idx;

	if (!tx->init)
		return;

	ptp_port = container_of(tx, struct ice_ptp_port, tx);
	pf = ptp_port_to_pf(ptp_port);
	hw = &pf->hw;

	/* Read the Tx ready status first */
	err = ice_get_phy_tx_tstamp_ready(hw, tx->block, &tstamp_ready);
	if (err)
		return;

	/* Drop packets if the link went down */
	link_up = ptp_port->link_up;

	for_each_set_bit(idx, tx->in_use, tx->len) {
		struct skb_shared_hwtstamps shhwtstamps = {};
		u8 phy_idx = idx + tx->offset;
		u64 raw_tstamp = 0, tstamp;
		bool drop_ts = !link_up;
		struct sk_buff *skb;

		/* Drop packets which have waited for more than 2 seconds */
		if (time_is_before_jiffies(tx->tstamps[idx].start + 2 * HZ)) {
			drop_ts = true;

			/* Count the number of Tx timestamps that timed out */
			pf->ptp.tx_hwtstamp_timeouts++;
		}

		/* Only read a timestamp from the PHY if its marked as ready
		 * by the tstamp_ready register. This avoids unnecessary
		 * reading of timestamps which are not yet valid. This is
		 * important as we must read all timestamps which are valid
		 * and only timestamps which are valid during each interrupt.
		 * If we do not, the hardware logic for generating a new
		 * interrupt can get stuck on some devices.
		 */
		if (!(tstamp_ready & BIT_ULL(phy_idx))) {
			if (drop_ts)
				goto skip_ts_read;

			continue;
		}

		ice_trace(tx_tstamp_fw_req, tx->tstamps[idx].skb, idx);

		err = ice_read_phy_tstamp(hw, tx->block, phy_idx, &raw_tstamp);
		if (err && !drop_ts)
			continue;

		ice_trace(tx_tstamp_fw_done, tx->tstamps[idx].skb, idx);

		/* For PHYs which don't implement a proper timestamp ready
		 * bitmap, verify that the timestamp value is different
		 * from the last cached timestamp. If it is not, skip this for
		 * now assuming it hasn't yet been captured by hardware.
		 */
		if (!drop_ts && tx->verify_cached &&
		    raw_tstamp == tx->tstamps[idx].cached_tstamp)
			continue;

		/* Discard any timestamp value without the valid bit set */
		if (!(raw_tstamp & ICE_PTP_TS_VALID))
			drop_ts = true;

skip_ts_read:
		spin_lock(&tx->lock);
		if (tx->verify_cached && raw_tstamp)
			tx->tstamps[idx].cached_tstamp = raw_tstamp;
		clear_bit(idx, tx->in_use);
		skb = tx->tstamps[idx].skb;
		tx->tstamps[idx].skb = NULL;
		if (test_and_clear_bit(idx, tx->stale))
			drop_ts = true;
		spin_unlock(&tx->lock);

		/* It is unlikely but possible that the SKB will have been
		 * flushed at this point due to link change or teardown.
		 */
		if (!skb)
			continue;

		if (drop_ts) {
			dev_kfree_skb_any(skb);
			continue;
		}

		/* Extend the timestamp using cached PHC time */
		tstamp = ice_ptp_extend_40b_ts(pf, raw_tstamp);
		if (tstamp) {
			shhwtstamps.hwtstamp = ns_to_ktime(tstamp);
			ice_trace(tx_tstamp_complete, skb, idx);
		}

		skb_tstamp_tx(skb, &shhwtstamps);
		dev_kfree_skb_any(skb);
	}
}

/**
 * ice_ptp_tx_tstamp - Process Tx timestamps for this function.
 * @tx: Tx tracking structure to initialize
 *
 * Returns: ICE_TX_TSTAMP_WORK_PENDING if there are any outstanding incomplete
 * Tx timestamps, or ICE_TX_TSTAMP_WORK_DONE otherwise.
 */
static enum ice_tx_tstamp_work ice_ptp_tx_tstamp(struct ice_ptp_tx *tx)
{
	bool more_timestamps;

	if (!tx->init)
		return ICE_TX_TSTAMP_WORK_DONE;

	/* Process the Tx timestamp tracker */
	ice_ptp_process_tx_tstamp(tx);

	/* Check if there are outstanding Tx timestamps */
	spin_lock(&tx->lock);
	more_timestamps = tx->init && !bitmap_empty(tx->in_use, tx->len);
	spin_unlock(&tx->lock);

	if (more_timestamps)
		return ICE_TX_TSTAMP_WORK_PENDING;

	return ICE_TX_TSTAMP_WORK_DONE;
}

/**
 * ice_ptp_alloc_tx_tracker - Initialize tracking for Tx timestamps
 * @tx: Tx tracking structure to initialize
 *
 * Assumes that the length has already been initialized. Do not call directly,
 * use the ice_ptp_init_tx_* instead.
 */
static int
ice_ptp_alloc_tx_tracker(struct ice_ptp_tx *tx)
{
	unsigned long *in_use, *stale;
	struct ice_tx_tstamp *tstamps;

	tstamps = kcalloc(tx->len, sizeof(*tstamps), GFP_KERNEL);
	in_use = bitmap_zalloc(tx->len, GFP_KERNEL);
	stale = bitmap_zalloc(tx->len, GFP_KERNEL);

	if (!tstamps || !in_use || !stale) {
		kfree(tstamps);
		bitmap_free(in_use);
		bitmap_free(stale);

		return -ENOMEM;
	}

	tx->tstamps = tstamps;
	tx->in_use = in_use;
	tx->stale = stale;
	tx->init = 1;

	spin_lock_init(&tx->lock);

	return 0;
}

/**
 * ice_ptp_flush_tx_tracker - Flush any remaining timestamps from the tracker
 * @pf: Board private structure
 * @tx: the tracker to flush
 *
 * Called during teardown when a Tx tracker is being removed.
 */
static void
ice_ptp_flush_tx_tracker(struct ice_pf *pf, struct ice_ptp_tx *tx)
{
	struct ice_hw *hw = &pf->hw;
	u64 tstamp_ready;
	int err;
	u8 idx;

	err = ice_get_phy_tx_tstamp_ready(hw, tx->block, &tstamp_ready);
	if (err) {
		dev_dbg(ice_pf_to_dev(pf), "Failed to get the Tx tstamp ready bitmap for block %u, err %d\n",
			tx->block, err);

		/* If we fail to read the Tx timestamp ready bitmap just
		 * skip clearing the PHY timestamps.
		 */
		tstamp_ready = 0;
	}

	for_each_set_bit(idx, tx->in_use, tx->len) {
		u8 phy_idx = idx + tx->offset;
		struct sk_buff *skb;

		/* In case this timestamp is ready, we need to clear it. */
		if (!hw->reset_ongoing && (tstamp_ready & BIT_ULL(phy_idx)))
			ice_clear_phy_tstamp(hw, tx->block, phy_idx);

		spin_lock(&tx->lock);
		skb = tx->tstamps[idx].skb;
		tx->tstamps[idx].skb = NULL;
		clear_bit(idx, tx->in_use);
		clear_bit(idx, tx->stale);
		spin_unlock(&tx->lock);

		/* Count the number of Tx timestamps flushed */
		pf->ptp.tx_hwtstamp_flushed++;

		/* Free the SKB after we've cleared the bit */
		dev_kfree_skb_any(skb);
	}
}

/**
 * ice_ptp_mark_tx_tracker_stale - Mark unfinished timestamps as stale
 * @tx: the tracker to mark
 *
 * Mark currently outstanding Tx timestamps as stale. This prevents sending
 * their timestamp value to the stack. This is required to prevent extending
 * the 40bit hardware timestamp incorrectly.
 *
 * This should be called when the PTP clock is modified such as after a set
 * time request.
 */
static void
ice_ptp_mark_tx_tracker_stale(struct ice_ptp_tx *tx)
{
	spin_lock(&tx->lock);
	bitmap_or(tx->stale, tx->stale, tx->in_use, tx->len);
	spin_unlock(&tx->lock);
}

/**
 * ice_ptp_release_tx_tracker - Release allocated memory for Tx tracker
 * @pf: Board private structure
 * @tx: Tx tracking structure to release
 *
 * Free memory associated with the Tx timestamp tracker.
 */
static void
ice_ptp_release_tx_tracker(struct ice_pf *pf, struct ice_ptp_tx *tx)
{
	spin_lock(&tx->lock);
	tx->init = 0;
	spin_unlock(&tx->lock);

	/* wait for potentially outstanding interrupt to complete */
	synchronize_irq(pf->oicr_irq.virq);

	ice_ptp_flush_tx_tracker(pf, tx);

	kfree(tx->tstamps);
	tx->tstamps = NULL;

	bitmap_free(tx->in_use);
	tx->in_use = NULL;

	bitmap_free(tx->stale);
	tx->stale = NULL;

	tx->len = 0;
}

/**
 * ice_ptp_init_tx_e822 - Initialize tracking for Tx timestamps
 * @pf: Board private structure
 * @tx: the Tx tracking structure to initialize
 * @port: the port this structure tracks
 *
 * Initialize the Tx timestamp tracker for this port. For generic MAC devices,
 * the timestamp block is shared for all ports in the same quad. To avoid
 * ports using the same timestamp index, logically break the block of
 * registers into chunks based on the port number.
 */
static int
ice_ptp_init_tx_e822(struct ice_pf *pf, struct ice_ptp_tx *tx, u8 port)
{
	tx->block = port / ICE_PORTS_PER_QUAD;
	tx->offset = (port % ICE_PORTS_PER_QUAD) * INDEX_PER_PORT_E822;
	tx->len = INDEX_PER_PORT_E822;
	tx->verify_cached = 0;

	return ice_ptp_alloc_tx_tracker(tx);
}

/**
 * ice_ptp_init_tx_e810 - Initialize tracking for Tx timestamps
 * @pf: Board private structure
 * @tx: the Tx tracking structure to initialize
 *
 * Initialize the Tx timestamp tracker for this PF. For E810 devices, each
 * port has its own block of timestamps, independent of the other ports.
 */
static int
ice_ptp_init_tx_e810(struct ice_pf *pf, struct ice_ptp_tx *tx)
{
	tx->block = pf->hw.port_info->lport;
	tx->offset = 0;
	tx->len = INDEX_PER_PORT_E810;
	/* The E810 PHY does not provide a timestamp ready bitmap. Instead,
	 * verify new timestamps against cached copy of the last read
	 * timestamp.
	 */
	tx->verify_cached = 1;

	return ice_ptp_alloc_tx_tracker(tx);
}

/**
 * ice_ptp_update_cached_phctime - Update the cached PHC time values
 * @pf: Board specific private structure
 *
 * This function updates the system time values which are cached in the PF
 * structure and the Rx rings.
 *
 * This function must be called periodically to ensure that the cached value
 * is never more than 2 seconds old.
 *
 * Note that the cached copy in the PF PTP structure is always updated, even
 * if we can't update the copy in the Rx rings.
 *
 * Return:
 * * 0 - OK, successfully updated
 * * -EAGAIN - PF was busy, need to reschedule the update
 */
static int ice_ptp_update_cached_phctime(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	unsigned long update_before;
	u64 systime;
	int i;

	update_before = pf->ptp.cached_phc_jiffies + msecs_to_jiffies(2000);
	if (pf->ptp.cached_phc_time &&
	    time_is_before_jiffies(update_before)) {
		unsigned long time_taken = jiffies - pf->ptp.cached_phc_jiffies;

		dev_warn(dev, "%u msecs passed between update to cached PHC time\n",
			 jiffies_to_msecs(time_taken));
		pf->ptp.late_cached_phc_updates++;
	}

	/* Read the current PHC time */
	systime = ice_ptp_read_src_clk_reg(pf, NULL);

	/* Update the cached PHC time stored in the PF structure */
	WRITE_ONCE(pf->ptp.cached_phc_time, systime);
	WRITE_ONCE(pf->ptp.cached_phc_jiffies, jiffies);

	if (test_and_set_bit(ICE_CFG_BUSY, pf->state))
		return -EAGAIN;

	ice_for_each_vsi(pf, i) {
		struct ice_vsi *vsi = pf->vsi[i];
		int j;

		if (!vsi)
			continue;

		if (vsi->type != ICE_VSI_PF)
			continue;

		ice_for_each_rxq(vsi, j) {
			if (!vsi->rx_rings[j])
				continue;
			WRITE_ONCE(vsi->rx_rings[j]->cached_phctime, systime);
		}
	}
	clear_bit(ICE_CFG_BUSY, pf->state);

	return 0;
}

/**
 * ice_ptp_reset_cached_phctime - Reset cached PHC time after an update
 * @pf: Board specific private structure
 *
 * This function must be called when the cached PHC time is no longer valid,
 * such as after a time adjustment. It marks any currently outstanding Tx
 * timestamps as stale and updates the cached PHC time for both the PF and Rx
 * rings.
 *
 * If updating the PHC time cannot be done immediately, a warning message is
 * logged and the work item is scheduled immediately to minimize the window
 * with a wrong cached timestamp.
 */
static void ice_ptp_reset_cached_phctime(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	int err;

	/* Update the cached PHC time immediately if possible, otherwise
	 * schedule the work item to execute soon.
	 */
	err = ice_ptp_update_cached_phctime(pf);
	if (err) {
		/* If another thread is updating the Rx rings, we won't
		 * properly reset them here. This could lead to reporting of
		 * invalid timestamps, but there isn't much we can do.
		 */
		dev_warn(dev, "%s: ICE_CFG_BUSY, unable to immediately update cached PHC time\n",
			 __func__);

		/* Queue the work item to update the Rx rings when possible */
		kthread_queue_delayed_work(pf->ptp.kworker, &pf->ptp.work,
					   msecs_to_jiffies(10));
	}

	/* Mark any outstanding timestamps as stale, since they might have
	 * been captured in hardware before the time update. This could lead
	 * to us extending them with the wrong cached value resulting in
	 * incorrect timestamp values.
	 */
	ice_ptp_mark_tx_tracker_stale(&pf->ptp.port.tx);
}

/**
 * ice_ptp_read_time - Read the time from the device
 * @pf: Board private structure
 * @ts: timespec structure to hold the current time value
 * @sts: Optional parameter for holding a pair of system timestamps from
 *       the system clock. Will be ignored if NULL is given.
 *
 * This function reads the source clock registers and stores them in a timespec.
 * However, since the registers are 64 bits of nanoseconds, we must convert the
 * result to a timespec before we can return.
 */
static void
ice_ptp_read_time(struct ice_pf *pf, struct timespec64 *ts,
		  struct ptp_system_timestamp *sts)
{
	u64 time_ns = ice_ptp_read_src_clk_reg(pf, sts);

	*ts = ns_to_timespec64(time_ns);
}

/**
 * ice_ptp_write_init - Set PHC time to provided value
 * @pf: Board private structure
 * @ts: timespec structure that holds the new time value
 *
 * Set the PHC time to the specified time provided in the timespec.
 */
static int ice_ptp_write_init(struct ice_pf *pf, struct timespec64 *ts)
{
	u64 ns = timespec64_to_ns(ts);
	struct ice_hw *hw = &pf->hw;

	return ice_ptp_init_time(hw, ns);
}

/**
 * ice_ptp_write_adj - Adjust PHC clock time atomically
 * @pf: Board private structure
 * @adj: Adjustment in nanoseconds
 *
 * Perform an atomic adjustment of the PHC time by the specified number of
 * nanoseconds.
 */
static int ice_ptp_write_adj(struct ice_pf *pf, s32 adj)
{
	struct ice_hw *hw = &pf->hw;

	return ice_ptp_adj_clock(hw, adj);
}

/**
 * ice_base_incval - Get base timer increment value
 * @pf: Board private structure
 *
 * Look up the base timer increment value for this device. The base increment
 * value is used to define the nominal clock tick rate. This increment value
 * is programmed during device initialization. It is also used as the basis
 * for calculating adjustments using scaled_ppm.
 */
static u64 ice_base_incval(struct ice_pf *pf)
{
	struct ice_hw *hw = &pf->hw;
	u64 incval;

	if (ice_is_e810(hw))
		incval = ICE_PTP_NOMINAL_INCVAL_E810;
	else if (ice_e822_time_ref(hw) < NUM_ICE_TIME_REF_FREQ)
		incval = ice_e822_nominal_incval(ice_e822_time_ref(hw));
	else
		incval = UNKNOWN_INCVAL_E822;

	dev_dbg(ice_pf_to_dev(pf), "PTP: using base increment value of 0x%016llx\n",
		incval);

	return incval;
}

/**
 * ice_ptp_check_tx_fifo - Check whether Tx FIFO is in an OK state
 * @port: PTP port for which Tx FIFO is checked
 */
static int ice_ptp_check_tx_fifo(struct ice_ptp_port *port)
{
	int quad = port->port_num / ICE_PORTS_PER_QUAD;
	int offs = port->port_num % ICE_PORTS_PER_QUAD;
	struct ice_pf *pf;
	struct ice_hw *hw;
	u32 val, phy_sts;
	int err;

	pf = ptp_port_to_pf(port);
	hw = &pf->hw;

	if (port->tx_fifo_busy_cnt == FIFO_OK)
		return 0;

	/* need to read FIFO state */
	if (offs == 0 || offs == 1)
		err = ice_read_quad_reg_e822(hw, quad, Q_REG_FIFO01_STATUS,
					     &val);
	else
		err = ice_read_quad_reg_e822(hw, quad, Q_REG_FIFO23_STATUS,
					     &val);

	if (err) {
		dev_err(ice_pf_to_dev(pf), "PTP failed to check port %d Tx FIFO, err %d\n",
			port->port_num, err);
		return err;
	}

	if (offs & 0x1)
		phy_sts = (val & Q_REG_FIFO13_M) >> Q_REG_FIFO13_S;
	else
		phy_sts = (val & Q_REG_FIFO02_M) >> Q_REG_FIFO02_S;

	if (phy_sts & FIFO_EMPTY) {
		port->tx_fifo_busy_cnt = FIFO_OK;
		return 0;
	}

	port->tx_fifo_busy_cnt++;

	dev_dbg(ice_pf_to_dev(pf), "Try %d, port %d FIFO not empty\n",
		port->tx_fifo_busy_cnt, port->port_num);

	if (port->tx_fifo_busy_cnt == ICE_PTP_FIFO_NUM_CHECKS) {
		dev_dbg(ice_pf_to_dev(pf),
			"Port %d Tx FIFO still not empty; resetting quad %d\n",
			port->port_num, quad);
		ice_ptp_reset_ts_memory_quad_e822(hw, quad);
		port->tx_fifo_busy_cnt = FIFO_OK;
		return 0;
	}

	return -EAGAIN;
}

/**
 * ice_ptp_wait_for_offsets - Check for valid Tx and Rx offsets
 * @work: Pointer to the kthread_work structure for this task
 *
 * Check whether hardware has completed measuring the Tx and Rx offset values
 * used to configure and enable vernier timestamp calibration.
 *
 * Once the offset in either direction is measured, configure the associated
 * registers with the calibrated offset values and enable timestamping. The Tx
 * and Rx directions are configured independently as soon as their associated
 * offsets are known.
 *
 * This function reschedules itself until both Tx and Rx calibration have
 * completed.
 */
static void ice_ptp_wait_for_offsets(struct kthread_work *work)
{
	struct ice_ptp_port *port;
	struct ice_pf *pf;
	struct ice_hw *hw;
	int tx_err;
	int rx_err;

	port = container_of(work, struct ice_ptp_port, ov_work.work);
	pf = ptp_port_to_pf(port);
	hw = &pf->hw;

	if (ice_is_reset_in_progress(pf->state)) {
		/* wait for device driver to complete reset */
		kthread_queue_delayed_work(pf->ptp.kworker,
					   &port->ov_work,
					   msecs_to_jiffies(100));
		return;
	}

	tx_err = ice_ptp_check_tx_fifo(port);
	if (!tx_err)
		tx_err = ice_phy_cfg_tx_offset_e822(hw, port->port_num);
	rx_err = ice_phy_cfg_rx_offset_e822(hw, port->port_num);
	if (tx_err || rx_err) {
		/* Tx and/or Rx offset not yet configured, try again later */
		kthread_queue_delayed_work(pf->ptp.kworker,
					   &port->ov_work,
					   msecs_to_jiffies(100));
		return;
	}
}

/**
 * ice_ptp_port_phy_stop - Stop timestamping for a PHY port
 * @ptp_port: PTP port to stop
 */
static int
ice_ptp_port_phy_stop(struct ice_ptp_port *ptp_port)
{
	struct ice_pf *pf = ptp_port_to_pf(ptp_port);
	u8 port = ptp_port->port_num;
	struct ice_hw *hw = &pf->hw;
	int err;

	if (ice_is_e810(hw))
		return 0;

	mutex_lock(&ptp_port->ps_lock);

	kthread_cancel_delayed_work_sync(&ptp_port->ov_work);

	err = ice_stop_phy_timer_e822(hw, port, true);
	if (err)
		dev_err(ice_pf_to_dev(pf), "PTP failed to set PHY port %d down, err %d\n",
			port, err);

	mutex_unlock(&ptp_port->ps_lock);

	return err;
}

/**
 * ice_ptp_port_phy_restart - (Re)start and calibrate PHY timestamping
 * @ptp_port: PTP port for which the PHY start is set
 *
 * Start the PHY timestamping block, and initiate Vernier timestamping
 * calibration. If timestamping cannot be calibrated (such as if link is down)
 * then disable the timestamping block instead.
 */
static int
ice_ptp_port_phy_restart(struct ice_ptp_port *ptp_port)
{
	struct ice_pf *pf = ptp_port_to_pf(ptp_port);
	u8 port = ptp_port->port_num;
	struct ice_hw *hw = &pf->hw;
	int err;

	if (ice_is_e810(hw))
		return 0;

	if (!ptp_port->link_up)
		return ice_ptp_port_phy_stop(ptp_port);

	mutex_lock(&ptp_port->ps_lock);

	kthread_cancel_delayed_work_sync(&ptp_port->ov_work);

	/* temporarily disable Tx timestamps while calibrating PHY offset */
	spin_lock(&ptp_port->tx.lock);
	ptp_port->tx.calibrating = true;
	spin_unlock(&ptp_port->tx.lock);
	ptp_port->tx_fifo_busy_cnt = 0;

	/* Start the PHY timer in Vernier mode */
	err = ice_start_phy_timer_e822(hw, port);
	if (err)
		goto out_unlock;

	/* Enable Tx timestamps right away */
	spin_lock(&ptp_port->tx.lock);
	ptp_port->tx.calibrating = false;
	spin_unlock(&ptp_port->tx.lock);

	kthread_queue_delayed_work(pf->ptp.kworker, &ptp_port->ov_work, 0);

out_unlock:
	if (err)
		dev_err(ice_pf_to_dev(pf), "PTP failed to set PHY port %d up, err %d\n",
			port, err);

	mutex_unlock(&ptp_port->ps_lock);

	return err;
}

/**
 * ice_ptp_link_change - Reconfigure PTP after link status change
 * @pf: Board private structure
 * @port: Port for which the PHY start is set
 * @linkup: Link is up or down
 */
void ice_ptp_link_change(struct ice_pf *pf, u8 port, bool linkup)
{
	struct ice_ptp_port *ptp_port;

	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return;

	if (WARN_ON_ONCE(port >= ICE_NUM_EXTERNAL_PORTS))
		return;

	ptp_port = &pf->ptp.port;
	if (WARN_ON_ONCE(ptp_port->port_num != port))
		return;

	/* Update cached link status for this port immediately */
	ptp_port->link_up = linkup;

	/* E810 devices do not need to reconfigure the PHY */
	if (ice_is_e810(&pf->hw))
		return;

	ice_ptp_port_phy_restart(ptp_port);
}

/**
 * ice_ptp_tx_ena_intr - Enable or disable the Tx timestamp interrupt
 * @pf: PF private structure
 * @ena: bool value to enable or disable interrupt
 * @threshold: Minimum number of packets at which intr is triggered
 *
 * Utility function to enable or disable Tx timestamp interrupt and threshold
 */
static int ice_ptp_tx_ena_intr(struct ice_pf *pf, bool ena, u32 threshold)
{
	struct ice_hw *hw = &pf->hw;
	int err = 0;
	int quad;
	u32 val;

	ice_ptp_reset_ts_memory(hw);

	for (quad = 0; quad < ICE_MAX_QUAD; quad++) {
		err = ice_read_quad_reg_e822(hw, quad, Q_REG_TX_MEM_GBL_CFG,
					     &val);
		if (err)
			break;

		if (ena) {
			val |= Q_REG_TX_MEM_GBL_CFG_INTR_ENA_M;
			val &= ~Q_REG_TX_MEM_GBL_CFG_INTR_THR_M;
			val |= ((threshold << Q_REG_TX_MEM_GBL_CFG_INTR_THR_S) &
				Q_REG_TX_MEM_GBL_CFG_INTR_THR_M);
		} else {
			val &= ~Q_REG_TX_MEM_GBL_CFG_INTR_ENA_M;
		}

		err = ice_write_quad_reg_e822(hw, quad, Q_REG_TX_MEM_GBL_CFG,
					      val);
		if (err)
			break;
	}

	if (err)
		dev_err(ice_pf_to_dev(pf), "PTP failed in intr ena, err %d\n",
			err);
	return err;
}

/**
 * ice_ptp_reset_phy_timestamping - Reset PHY timestamping block
 * @pf: Board private structure
 */
static void ice_ptp_reset_phy_timestamping(struct ice_pf *pf)
{
	ice_ptp_port_phy_restart(&pf->ptp.port);
}

/**
 * ice_ptp_adjfine - Adjust clock increment rate
 * @info: the driver's PTP info structure
 * @scaled_ppm: Parts per million with 16-bit fractional field
 *
 * Adjust the frequency of the clock by the indicated scaled ppm from the
 * base frequency.
 */
static int ice_ptp_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_hw *hw = &pf->hw;
	u64 incval;
	int err;

	incval = adjust_by_scaled_ppm(ice_base_incval(pf), scaled_ppm);
	err = ice_ptp_write_incval_locked(hw, incval);
	if (err) {
		dev_err(ice_pf_to_dev(pf), "PTP failed to set incval, err %d\n",
			err);
		return -EIO;
	}

	return 0;
}

/**
 * ice_ptp_extts_event - Process PTP external clock event
 * @pf: Board private structure
 */
void ice_ptp_extts_event(struct ice_pf *pf)
{
	struct ptp_clock_event event;
	struct ice_hw *hw = &pf->hw;
	u8 chan, tmr_idx;
	u32 hi, lo;

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_owned;
	/* Event time is captured by one of the two matched registers
	 *      GLTSYN_EVNT_L: 32 LSB of sampled time event
	 *      GLTSYN_EVNT_H: 32 MSB of sampled time event
	 * Event is defined in GLTSYN_EVNT_0 register
	 */
	for (chan = 0; chan < GLTSYN_EVNT_H_IDX_MAX; chan++) {
		/* Check if channel is enabled */
		if (pf->ptp.ext_ts_irq & (1 << chan)) {
			lo = rd32(hw, GLTSYN_EVNT_L(chan, tmr_idx));
			hi = rd32(hw, GLTSYN_EVNT_H(chan, tmr_idx));
			event.timestamp = (((u64)hi) << 32) | lo;
			event.type = PTP_CLOCK_EXTTS;
			event.index = chan;

			/* Fire event */
			ptp_clock_event(pf->ptp.clock, &event);
			pf->ptp.ext_ts_irq &= ~(1 << chan);
		}
	}
}

/**
 * ice_ptp_cfg_extts - Configure EXTTS pin and channel
 * @pf: Board private structure
 * @ena: true to enable; false to disable
 * @chan: GPIO channel (0-3)
 * @gpio_pin: GPIO pin
 * @extts_flags: request flags from the ptp_extts_request.flags
 */
static int
ice_ptp_cfg_extts(struct ice_pf *pf, bool ena, unsigned int chan, u32 gpio_pin,
		  unsigned int extts_flags)
{
	u32 func, aux_reg, gpio_reg, irq_reg;
	struct ice_hw *hw = &pf->hw;
	u8 tmr_idx;

	if (chan > (unsigned int)pf->ptp.info.n_ext_ts)
		return -EINVAL;

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_owned;

	irq_reg = rd32(hw, PFINT_OICR_ENA);

	if (ena) {
		/* Enable the interrupt */
		irq_reg |= PFINT_OICR_TSYN_EVNT_M;
		aux_reg = GLTSYN_AUX_IN_0_INT_ENA_M;

#define GLTSYN_AUX_IN_0_EVNTLVL_RISING_EDGE	BIT(0)
#define GLTSYN_AUX_IN_0_EVNTLVL_FALLING_EDGE	BIT(1)

		/* set event level to requested edge */
		if (extts_flags & PTP_FALLING_EDGE)
			aux_reg |= GLTSYN_AUX_IN_0_EVNTLVL_FALLING_EDGE;
		if (extts_flags & PTP_RISING_EDGE)
			aux_reg |= GLTSYN_AUX_IN_0_EVNTLVL_RISING_EDGE;

		/* Write GPIO CTL reg.
		 * 0x1 is input sampled by EVENT register(channel)
		 * + num_in_channels * tmr_idx
		 */
		func = 1 + chan + (tmr_idx * 3);
		gpio_reg = ((func << GLGEN_GPIO_CTL_PIN_FUNC_S) &
			    GLGEN_GPIO_CTL_PIN_FUNC_M);
		pf->ptp.ext_ts_chan |= (1 << chan);
	} else {
		/* clear the values we set to reset defaults */
		aux_reg = 0;
		gpio_reg = 0;
		pf->ptp.ext_ts_chan &= ~(1 << chan);
		if (!pf->ptp.ext_ts_chan)
			irq_reg &= ~PFINT_OICR_TSYN_EVNT_M;
	}

	wr32(hw, PFINT_OICR_ENA, irq_reg);
	wr32(hw, GLTSYN_AUX_IN(chan, tmr_idx), aux_reg);
	wr32(hw, GLGEN_GPIO_CTL(gpio_pin), gpio_reg);

	return 0;
}

/**
 * ice_ptp_cfg_clkout - Configure clock to generate periodic wave
 * @pf: Board private structure
 * @chan: GPIO channel (0-3)
 * @config: desired periodic clk configuration. NULL will disable channel
 * @store: If set to true the values will be stored
 *
 * Configure the internal clock generator modules to generate the clock wave of
 * specified period.
 */
static int ice_ptp_cfg_clkout(struct ice_pf *pf, unsigned int chan,
			      struct ice_perout_channel *config, bool store)
{
	u64 current_time, period, start_time, phase;
	struct ice_hw *hw = &pf->hw;
	u32 func, val, gpio_pin;
	u8 tmr_idx;

	tmr_idx = hw->func_caps.ts_func_info.tmr_index_owned;

	/* 0. Reset mode & out_en in AUX_OUT */
	wr32(hw, GLTSYN_AUX_OUT(chan, tmr_idx), 0);

	/* If we're disabling the output, clear out CLKO and TGT and keep
	 * output level low
	 */
	if (!config || !config->ena) {
		wr32(hw, GLTSYN_CLKO(chan, tmr_idx), 0);
		wr32(hw, GLTSYN_TGT_L(chan, tmr_idx), 0);
		wr32(hw, GLTSYN_TGT_H(chan, tmr_idx), 0);

		val = GLGEN_GPIO_CTL_PIN_DIR_M;
		gpio_pin = pf->ptp.perout_channels[chan].gpio_pin;
		wr32(hw, GLGEN_GPIO_CTL(gpio_pin), val);

		/* Store the value if requested */
		if (store)
			memset(&pf->ptp.perout_channels[chan], 0,
			       sizeof(struct ice_perout_channel));

		return 0;
	}
	period = config->period;
	start_time = config->start_time;
	div64_u64_rem(start_time, period, &phase);
	gpio_pin = config->gpio_pin;

	/* 1. Write clkout with half of required period value */
	if (period & 0x1) {
		dev_err(ice_pf_to_dev(pf), "CLK Period must be an even value\n");
		goto err;
	}

	period >>= 1;

	/* For proper operation, the GLTSYN_CLKO must be larger than clock tick
	 */
#define MIN_PULSE 3
	if (period <= MIN_PULSE || period > U32_MAX) {
		dev_err(ice_pf_to_dev(pf), "CLK Period must be > %d && < 2^33",
			MIN_PULSE * 2);
		goto err;
	}

	wr32(hw, GLTSYN_CLKO(chan, tmr_idx), lower_32_bits(period));

	/* Allow time for programming before start_time is hit */
	current_time = ice_ptp_read_src_clk_reg(pf, NULL);

	/* if start time is in the past start the timer at the nearest second
	 * maintaining phase
	 */
	if (start_time < current_time)
		start_time = div64_u64(current_time + NSEC_PER_SEC - 1,
				       NSEC_PER_SEC) * NSEC_PER_SEC + phase;

	if (ice_is_e810(hw))
		start_time -= E810_OUT_PROP_DELAY_NS;
	else
		start_time -= ice_e822_pps_delay(ice_e822_time_ref(hw));

	/* 2. Write TARGET time */
	wr32(hw, GLTSYN_TGT_L(chan, tmr_idx), lower_32_bits(start_time));
	wr32(hw, GLTSYN_TGT_H(chan, tmr_idx), upper_32_bits(start_time));

	/* 3. Write AUX_OUT register */
	val = GLTSYN_AUX_OUT_0_OUT_ENA_M | GLTSYN_AUX_OUT_0_OUTMOD_M;
	wr32(hw, GLTSYN_AUX_OUT(chan, tmr_idx), val);

	/* 4. write GPIO CTL reg */
	func = 8 + chan + (tmr_idx * 4);
	val = GLGEN_GPIO_CTL_PIN_DIR_M |
	      ((func << GLGEN_GPIO_CTL_PIN_FUNC_S) & GLGEN_GPIO_CTL_PIN_FUNC_M);
	wr32(hw, GLGEN_GPIO_CTL(gpio_pin), val);

	/* Store the value if requested */
	if (store) {
		memcpy(&pf->ptp.perout_channels[chan], config,
		       sizeof(struct ice_perout_channel));
		pf->ptp.perout_channels[chan].start_time = phase;
	}

	return 0;
err:
	dev_err(ice_pf_to_dev(pf), "PTP failed to cfg per_clk\n");
	return -EFAULT;
}

/**
 * ice_ptp_disable_all_clkout - Disable all currently configured outputs
 * @pf: pointer to the PF structure
 *
 * Disable all currently configured clock outputs. This is necessary before
 * certain changes to the PTP hardware clock. Use ice_ptp_enable_all_clkout to
 * re-enable the clocks again.
 */
static void ice_ptp_disable_all_clkout(struct ice_pf *pf)
{
	uint i;

	for (i = 0; i < pf->ptp.info.n_per_out; i++)
		if (pf->ptp.perout_channels[i].ena)
			ice_ptp_cfg_clkout(pf, i, NULL, false);
}

/**
 * ice_ptp_enable_all_clkout - Enable all configured periodic clock outputs
 * @pf: pointer to the PF structure
 *
 * Enable all currently configured clock outputs. Use this after
 * ice_ptp_disable_all_clkout to reconfigure the output signals according to
 * their configuration.
 */
static void ice_ptp_enable_all_clkout(struct ice_pf *pf)
{
	uint i;

	for (i = 0; i < pf->ptp.info.n_per_out; i++)
		if (pf->ptp.perout_channels[i].ena)
			ice_ptp_cfg_clkout(pf, i, &pf->ptp.perout_channels[i],
					   false);
}

/**
 * ice_ptp_gpio_enable_e810 - Enable/disable ancillary features of PHC
 * @info: the driver's PTP info structure
 * @rq: The requested feature to change
 * @on: Enable/disable flag
 */
static int
ice_ptp_gpio_enable_e810(struct ptp_clock_info *info,
			 struct ptp_clock_request *rq, int on)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_perout_channel clk_cfg = {0};
	bool sma_pres = false;
	unsigned int chan;
	u32 gpio_pin;
	int err;

	if (ice_is_feature_supported(pf, ICE_F_SMA_CTRL))
		sma_pres = true;

	switch (rq->type) {
	case PTP_CLK_REQ_PEROUT:
		chan = rq->perout.index;
		if (sma_pres) {
			if (chan == ice_pin_desc_e810t[SMA1].chan)
				clk_cfg.gpio_pin = GPIO_20;
			else if (chan == ice_pin_desc_e810t[SMA2].chan)
				clk_cfg.gpio_pin = GPIO_22;
			else
				return -1;
		} else if (ice_is_e810t(&pf->hw)) {
			if (chan == 0)
				clk_cfg.gpio_pin = GPIO_20;
			else
				clk_cfg.gpio_pin = GPIO_22;
		} else if (chan == PPS_CLK_GEN_CHAN) {
			clk_cfg.gpio_pin = PPS_PIN_INDEX;
		} else {
			clk_cfg.gpio_pin = chan;
		}

		clk_cfg.period = ((rq->perout.period.sec * NSEC_PER_SEC) +
				   rq->perout.period.nsec);
		clk_cfg.start_time = ((rq->perout.start.sec * NSEC_PER_SEC) +
				       rq->perout.start.nsec);
		clk_cfg.ena = !!on;

		err = ice_ptp_cfg_clkout(pf, chan, &clk_cfg, true);
		break;
	case PTP_CLK_REQ_EXTTS:
		chan = rq->extts.index;
		if (sma_pres) {
			if (chan < ice_pin_desc_e810t[SMA2].chan)
				gpio_pin = GPIO_21;
			else
				gpio_pin = GPIO_23;
		} else if (ice_is_e810t(&pf->hw)) {
			if (chan == 0)
				gpio_pin = GPIO_21;
			else
				gpio_pin = GPIO_23;
		} else {
			gpio_pin = chan;
		}

		err = ice_ptp_cfg_extts(pf, !!on, chan, gpio_pin,
					rq->extts.flags);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

/**
 * ice_ptp_gpio_enable_e823 - Enable/disable ancillary features of PHC
 * @info: the driver's PTP info structure
 * @rq: The requested feature to change
 * @on: Enable/disable flag
 */
static int ice_ptp_gpio_enable_e823(struct ptp_clock_info *info,
				    struct ptp_clock_request *rq, int on)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_perout_channel clk_cfg = {0};
	int err;

	switch (rq->type) {
	case PTP_CLK_REQ_PPS:
		clk_cfg.gpio_pin = PPS_PIN_INDEX;
		clk_cfg.period = NSEC_PER_SEC;
		clk_cfg.ena = !!on;

		err = ice_ptp_cfg_clkout(pf, PPS_CLK_GEN_CHAN, &clk_cfg, true);
		break;
	case PTP_CLK_REQ_EXTTS:
		err = ice_ptp_cfg_extts(pf, !!on, rq->extts.index,
					TIME_SYNC_PIN_INDEX, rq->extts.flags);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

/**
 * ice_ptp_gettimex64 - Get the time of the clock
 * @info: the driver's PTP info structure
 * @ts: timespec64 structure to hold the current time value
 * @sts: Optional parameter for holding a pair of system timestamps from
 *       the system clock. Will be ignored if NULL is given.
 *
 * Read the device clock and return the correct value on ns, after converting it
 * into a timespec struct.
 */
static int
ice_ptp_gettimex64(struct ptp_clock_info *info, struct timespec64 *ts,
		   struct ptp_system_timestamp *sts)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_hw *hw = &pf->hw;

	if (!ice_ptp_lock(hw)) {
		dev_err(ice_pf_to_dev(pf), "PTP failed to get time\n");
		return -EBUSY;
	}

	ice_ptp_read_time(pf, ts, sts);
	ice_ptp_unlock(hw);

	return 0;
}

/**
 * ice_ptp_settime64 - Set the time of the clock
 * @info: the driver's PTP info structure
 * @ts: timespec64 structure that holds the new time value
 *
 * Set the device clock to the user input value. The conversion from timespec
 * to ns happens in the write function.
 */
static int
ice_ptp_settime64(struct ptp_clock_info *info, const struct timespec64 *ts)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct timespec64 ts64 = *ts;
	struct ice_hw *hw = &pf->hw;
	int err;

	/* For Vernier mode, we need to recalibrate after new settime
	 * Start with disabling timestamp block
	 */
	if (pf->ptp.port.link_up)
		ice_ptp_port_phy_stop(&pf->ptp.port);

	if (!ice_ptp_lock(hw)) {
		err = -EBUSY;
		goto exit;
	}

	/* Disable periodic outputs */
	ice_ptp_disable_all_clkout(pf);

	err = ice_ptp_write_init(pf, &ts64);
	ice_ptp_unlock(hw);

	if (!err)
		ice_ptp_reset_cached_phctime(pf);

	/* Reenable periodic outputs */
	ice_ptp_enable_all_clkout(pf);

	/* Recalibrate and re-enable timestamp block */
	if (pf->ptp.port.link_up)
		ice_ptp_port_phy_restart(&pf->ptp.port);
exit:
	if (err) {
		dev_err(ice_pf_to_dev(pf), "PTP failed to set time %d\n", err);
		return err;
	}

	return 0;
}

/**
 * ice_ptp_adjtime_nonatomic - Do a non-atomic clock adjustment
 * @info: the driver's PTP info structure
 * @delta: Offset in nanoseconds to adjust the time by
 */
static int ice_ptp_adjtime_nonatomic(struct ptp_clock_info *info, s64 delta)
{
	struct timespec64 now, then;
	int ret;

	then = ns_to_timespec64(delta);
	ret = ice_ptp_gettimex64(info, &now, NULL);
	if (ret)
		return ret;
	now = timespec64_add(now, then);

	return ice_ptp_settime64(info, (const struct timespec64 *)&now);
}

/**
 * ice_ptp_adjtime - Adjust the time of the clock by the indicated delta
 * @info: the driver's PTP info structure
 * @delta: Offset in nanoseconds to adjust the time by
 */
static int ice_ptp_adjtime(struct ptp_clock_info *info, s64 delta)
{
	struct ice_pf *pf = ptp_info_to_pf(info);
	struct ice_hw *hw = &pf->hw;
	struct device *dev;
	int err;

	dev = ice_pf_to_dev(pf);

	/* Hardware only supports atomic adjustments using signed 32-bit
	 * integers. For any adjustment outside this range, perform
	 * a non-atomic get->adjust->set flow.
	 */
	if (delta > S32_MAX || delta < S32_MIN) {
		dev_dbg(dev, "delta = %lld, adjtime non-atomic\n", delta);
		return ice_ptp_adjtime_nonatomic(info, delta);
	}

	if (!ice_ptp_lock(hw)) {
		dev_err(dev, "PTP failed to acquire semaphore in adjtime\n");
		return -EBUSY;
	}

	/* Disable periodic outputs */
	ice_ptp_disable_all_clkout(pf);

	err = ice_ptp_write_adj(pf, delta);

	/* Reenable periodic outputs */
	ice_ptp_enable_all_clkout(pf);

	ice_ptp_unlock(hw);

	if (err) {
		dev_err(dev, "PTP failed to adjust time, err %d\n", err);
		return err;
	}

	ice_ptp_reset_cached_phctime(pf);

	return 0;
}

#ifdef CONFIG_ICE_HWTS
/**
 * ice_ptp_get_syncdevicetime - Get the cross time stamp info
 * @device: Current device time
 * @system: System counter value read synchronously with device time
 * @ctx: Context provided by timekeeping code
 *
 * Read device and system (ART) clock simultaneously and return the corrected
 * clock values in ns.
 */
static int
ice_ptp_get_syncdevicetime(ktime_t *device,
			   struct system_counterval_t *system,
			   void *ctx)
{
	struct ice_pf *pf = (struct ice_pf *)ctx;
	struct ice_hw *hw = &pf->hw;
	u32 hh_lock, hh_art_ctl;
	int i;

	/* Get the HW lock */
	hh_lock = rd32(hw, PFHH_SEM + (PFTSYN_SEM_BYTES * hw->pf_id));
	if (hh_lock & PFHH_SEM_BUSY_M) {
		dev_err(ice_pf_to_dev(pf), "PTP failed to get hh lock\n");
		return -EFAULT;
	}

	/* Start the ART and device clock sync sequence */
	hh_art_ctl = rd32(hw, GLHH_ART_CTL);
	hh_art_ctl = hh_art_ctl | GLHH_ART_CTL_ACTIVE_M;
	wr32(hw, GLHH_ART_CTL, hh_art_ctl);

#define MAX_HH_LOCK_TRIES 100

	for (i = 0; i < MAX_HH_LOCK_TRIES; i++) {
		/* Wait for sync to complete */
		hh_art_ctl = rd32(hw, GLHH_ART_CTL);
		if (hh_art_ctl & GLHH_ART_CTL_ACTIVE_M) {
			udelay(1);
			continue;
		} else {
			u32 hh_ts_lo, hh_ts_hi, tmr_idx;
			u64 hh_ts;

			tmr_idx = hw->func_caps.ts_func_info.tmr_index_assoc;
			/* Read ART time */
			hh_ts_lo = rd32(hw, GLHH_ART_TIME_L);
			hh_ts_hi = rd32(hw, GLHH_ART_TIME_H);
			hh_ts = ((u64)hh_ts_hi << 32) | hh_ts_lo;
			*system = convert_art_ns_to_tsc(hh_ts);
			/* Read Device source clock time */
			hh_ts_lo = rd32(hw, GLTSYN_HHTIME_L(tmr_idx));
			hh_ts_hi = rd32(hw, GLTSYN_HHTIME_H(tmr_idx));
			hh_ts = ((u64)hh_ts_hi << 32) | hh_ts_lo;
			*device = ns_to_ktime(hh_ts);
			break;
		}
	}
	/* Release HW lock */
	hh_lock = rd32(hw, PFHH_SEM + (PFTSYN_SEM_BYTES * hw->pf_id));
	hh_lock = hh_lock & ~PFHH_SEM_BUSY_M;
	wr32(hw, PFHH_SEM + (PFTSYN_SEM_BYTES * hw->pf_id), hh_lock);

	if (i == MAX_HH_LOCK_TRIES)
		return -ETIMEDOUT;

	return 0;
}

/**
 * ice_ptp_getcrosststamp_e822 - Capture a device cross timestamp
 * @info: the driver's PTP info structure
 * @cts: The memory to fill the cross timestamp info
 *
 * Capture a cross timestamp between the ART and the device PTP hardware
 * clock. Fill the cross timestamp information and report it back to the
 * caller.
 *
 * This is only valid for E822 devices which have support for generating the
 * cross timestamp via PCIe PTM.
 *
 * In order to correctly correlate the ART timestamp back to the TSC time, the
 * CPU must have X86_FEATURE_TSC_KNOWN_FREQ.
 */
static int
ice_ptp_getcrosststamp_e822(struct ptp_clock_info *info,
			    struct system_device_crosststamp *cts)
{
	struct ice_pf *pf = ptp_info_to_pf(info);

	return get_device_system_crosststamp(ice_ptp_get_syncdevicetime,
					     pf, NULL, cts);
}
#endif /* CONFIG_ICE_HWTS */

/**
 * ice_ptp_get_ts_config - ioctl interface to read the timestamping config
 * @pf: Board private structure
 * @ifr: ioctl data
 *
 * Copy the timestamping config to user buffer
 */
int ice_ptp_get_ts_config(struct ice_pf *pf, struct ifreq *ifr)
{
	struct hwtstamp_config *config;

	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return -EIO;

	config = &pf->ptp.tstamp_config;

	return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ?
		-EFAULT : 0;
}

/**
 * ice_ptp_set_timestamp_mode - Setup driver for requested timestamp mode
 * @pf: Board private structure
 * @config: hwtstamp settings requested or saved
 */
static int
ice_ptp_set_timestamp_mode(struct ice_pf *pf, struct hwtstamp_config *config)
{
	switch (config->tx_type) {
	case HWTSTAMP_TX_OFF:
		ice_set_tx_tstamp(pf, false);
		break;
	case HWTSTAMP_TX_ON:
		ice_set_tx_tstamp(pf, true);
		break;
	default:
		return -ERANGE;
	}

	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		ice_set_rx_tstamp(pf, false);
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_NTP_ALL:
	case HWTSTAMP_FILTER_ALL:
		ice_set_rx_tstamp(pf, true);
		break;
	default:
		return -ERANGE;
	}

	return 0;
}

/**
 * ice_ptp_set_ts_config - ioctl interface to control the timestamping
 * @pf: Board private structure
 * @ifr: ioctl data
 *
 * Get the user config and store it
 */
int ice_ptp_set_ts_config(struct ice_pf *pf, struct ifreq *ifr)
{
	struct hwtstamp_config config;
	int err;

	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return -EAGAIN;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	err = ice_ptp_set_timestamp_mode(pf, &config);
	if (err)
		return err;

	/* Return the actual configuration set */
	config = pf->ptp.tstamp_config;

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

/**
 * ice_ptp_rx_hwtstamp - Check for an Rx timestamp
 * @rx_ring: Ring to get the VSI info
 * @rx_desc: Receive descriptor
 * @skb: Particular skb to send timestamp with
 *
 * The driver receives a notification in the receive descriptor with timestamp.
 * The timestamp is in ns, so we must convert the result first.
 */
void
ice_ptp_rx_hwtstamp(struct ice_rx_ring *rx_ring,
		    union ice_32b_rx_flex_desc *rx_desc, struct sk_buff *skb)
{
	struct skb_shared_hwtstamps *hwtstamps;
	u64 ts_ns, cached_time;
	u32 ts_high;

	if (!(rx_desc->wb.time_stamp_low & ICE_PTP_TS_VALID))
		return;

	cached_time = READ_ONCE(rx_ring->cached_phctime);

	/* Do not report a timestamp if we don't have a cached PHC time */
	if (!cached_time)
		return;

	/* Use ice_ptp_extend_32b_ts directly, using the ring-specific cached
	 * PHC value, rather than accessing the PF. This also allows us to
	 * simply pass the upper 32bits of nanoseconds directly. Calling
	 * ice_ptp_extend_40b_ts is unnecessary as it would just discard these
	 * bits itself.
	 */
	ts_high = le32_to_cpu(rx_desc->wb.flex_ts.ts_high);
	ts_ns = ice_ptp_extend_32b_ts(cached_time, ts_high);

	hwtstamps = skb_hwtstamps(skb);
	memset(hwtstamps, 0, sizeof(*hwtstamps));
	hwtstamps->hwtstamp = ns_to_ktime(ts_ns);
}

/**
 * ice_ptp_disable_sma_pins_e810t - Disable E810-T SMA pins
 * @pf: pointer to the PF structure
 * @info: PTP clock info structure
 *
 * Disable the OS access to the SMA pins. Called to clear out the OS
 * indications of pin support when we fail to setup the E810-T SMA control
 * register.
 */
static void
ice_ptp_disable_sma_pins_e810t(struct ice_pf *pf, struct ptp_clock_info *info)
{
	struct device *dev = ice_pf_to_dev(pf);

	dev_warn(dev, "Failed to configure E810-T SMA pin control\n");

	info->enable = NULL;
	info->verify = NULL;
	info->n_pins = 0;
	info->n_ext_ts = 0;
	info->n_per_out = 0;
}

/**
 * ice_ptp_setup_sma_pins_e810t - Setup the SMA pins
 * @pf: pointer to the PF structure
 * @info: PTP clock info structure
 *
 * Finish setting up the SMA pins by allocating pin_config, and setting it up
 * according to the current status of the SMA. On failure, disable all of the
 * extended SMA pin support.
 */
static void
ice_ptp_setup_sma_pins_e810t(struct ice_pf *pf, struct ptp_clock_info *info)
{
	struct device *dev = ice_pf_to_dev(pf);
	int err;

	/* Allocate memory for kernel pins interface */
	info->pin_config = devm_kcalloc(dev, info->n_pins,
					sizeof(*info->pin_config), GFP_KERNEL);
	if (!info->pin_config) {
		ice_ptp_disable_sma_pins_e810t(pf, info);
		return;
	}

	/* Read current SMA status */
	err = ice_get_sma_config_e810t(&pf->hw, info->pin_config);
	if (err)
		ice_ptp_disable_sma_pins_e810t(pf, info);
}

/**
 * ice_ptp_setup_pins_e810 - Setup PTP pins in sysfs
 * @pf: pointer to the PF instance
 * @info: PTP clock capabilities
 */
static void
ice_ptp_setup_pins_e810(struct ice_pf *pf, struct ptp_clock_info *info)
{
	info->n_per_out = N_PER_OUT_E810;

	if (ice_is_feature_supported(pf, ICE_F_PTP_EXTTS))
		info->n_ext_ts = N_EXT_TS_E810;

	if (ice_is_feature_supported(pf, ICE_F_SMA_CTRL)) {
		info->n_ext_ts = N_EXT_TS_E810;
		info->n_pins = NUM_PTP_PINS_E810T;
		info->verify = ice_verify_pin_e810t;

		/* Complete setup of the SMA pins */
		ice_ptp_setup_sma_pins_e810t(pf, info);
	}
}

/**
 * ice_ptp_setup_pins_e823 - Setup PTP pins in sysfs
 * @pf: pointer to the PF instance
 * @info: PTP clock capabilities
 */
static void
ice_ptp_setup_pins_e823(struct ice_pf *pf, struct ptp_clock_info *info)
{
	info->pps = 1;
	info->n_per_out = 0;
	info->n_ext_ts = 1;
}

/**
 * ice_ptp_set_funcs_e822 - Set specialized functions for E822 support
 * @pf: Board private structure
 * @info: PTP info to fill
 *
 * Assign functions to the PTP capabiltiies structure for E822 devices.
 * Functions which operate across all device families should be set directly
 * in ice_ptp_set_caps. Only add functions here which are distinct for E822
 * devices.
 */
static void
ice_ptp_set_funcs_e822(struct ice_pf *pf, struct ptp_clock_info *info)
{
#ifdef CONFIG_ICE_HWTS
	if (boot_cpu_has(X86_FEATURE_ART) &&
	    boot_cpu_has(X86_FEATURE_TSC_KNOWN_FREQ))
		info->getcrosststamp = ice_ptp_getcrosststamp_e822;
#endif /* CONFIG_ICE_HWTS */
}

/**
 * ice_ptp_set_funcs_e810 - Set specialized functions for E810 support
 * @pf: Board private structure
 * @info: PTP info to fill
 *
 * Assign functions to the PTP capabiltiies structure for E810 devices.
 * Functions which operate across all device families should be set directly
 * in ice_ptp_set_caps. Only add functions here which are distinct for e810
 * devices.
 */
static void
ice_ptp_set_funcs_e810(struct ice_pf *pf, struct ptp_clock_info *info)
{
	info->enable = ice_ptp_gpio_enable_e810;
	ice_ptp_setup_pins_e810(pf, info);
}

/**
 * ice_ptp_set_funcs_e823 - Set specialized functions for E823 support
 * @pf: Board private structure
 * @info: PTP info to fill
 *
 * Assign functions to the PTP capabiltiies structure for E823 devices.
 * Functions which operate across all device families should be set directly
 * in ice_ptp_set_caps. Only add functions here which are distinct for e823
 * devices.
 */
static void
ice_ptp_set_funcs_e823(struct ice_pf *pf, struct ptp_clock_info *info)
{
	info->enable = ice_ptp_gpio_enable_e823;
	ice_ptp_setup_pins_e823(pf, info);
}

/**
 * ice_ptp_set_caps - Set PTP capabilities
 * @pf: Board private structure
 */
static void ice_ptp_set_caps(struct ice_pf *pf)
{
	struct ptp_clock_info *info = &pf->ptp.info;
	struct device *dev = ice_pf_to_dev(pf);

	snprintf(info->name, sizeof(info->name) - 1, "%s-%s-clk",
		 dev_driver_string(dev), dev_name(dev));
	info->owner = THIS_MODULE;
	info->max_adj = 100000000;
	info->adjtime = ice_ptp_adjtime;
	info->adjfine = ice_ptp_adjfine;
	info->gettimex64 = ice_ptp_gettimex64;
	info->settime64 = ice_ptp_settime64;

	if (ice_is_e810(&pf->hw))
		ice_ptp_set_funcs_e810(pf, info);
	else if (ice_is_e823(&pf->hw))
		ice_ptp_set_funcs_e823(pf, info);
	else
		ice_ptp_set_funcs_e822(pf, info);
}

/**
 * ice_ptp_create_clock - Create PTP clock device for userspace
 * @pf: Board private structure
 *
 * This function creates a new PTP clock device. It only creates one if we
 * don't already have one. Will return error if it can't create one, but success
 * if we already have a device. Should be used by ice_ptp_init to create clock
 * initially, and prevent global resets from creating new clock devices.
 */
static long ice_ptp_create_clock(struct ice_pf *pf)
{
	struct ptp_clock_info *info;
	struct ptp_clock *clock;
	struct device *dev;

	/* No need to create a clock device if we already have one */
	if (pf->ptp.clock)
		return 0;

	ice_ptp_set_caps(pf);

	info = &pf->ptp.info;
	dev = ice_pf_to_dev(pf);

	/* Attempt to register the clock before enabling the hardware. */
	clock = ptp_clock_register(info, dev);
	if (IS_ERR(clock))
		return PTR_ERR(clock);

	pf->ptp.clock = clock;

	return 0;
}

/**
 * ice_ptp_request_ts - Request an available Tx timestamp index
 * @tx: the PTP Tx timestamp tracker to request from
 * @skb: the SKB to associate with this timestamp request
 */
s8 ice_ptp_request_ts(struct ice_ptp_tx *tx, struct sk_buff *skb)
{
	u8 idx;

	spin_lock(&tx->lock);

	/* Check that this tracker is accepting new timestamp requests */
	if (!ice_ptp_is_tx_tracker_up(tx)) {
		spin_unlock(&tx->lock);
		return -1;
	}

	/* Find and set the first available index */
	idx = find_first_zero_bit(tx->in_use, tx->len);
	if (idx < tx->len) {
		/* We got a valid index that no other thread could have set. Store
		 * a reference to the skb and the start time to allow discarding old
		 * requests.
		 */
		set_bit(idx, tx->in_use);
		clear_bit(idx, tx->stale);
		tx->tstamps[idx].start = jiffies;
		tx->tstamps[idx].skb = skb_get(skb);
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		ice_trace(tx_tstamp_request, skb, idx);
	}

	spin_unlock(&tx->lock);

	/* return the appropriate PHY timestamp register index, -1 if no
	 * indexes were available.
	 */
	if (idx >= tx->len)
		return -1;
	else
		return idx + tx->offset;
}

/**
 * ice_ptp_process_ts - Process the PTP Tx timestamps
 * @pf: Board private structure
 *
 * Returns: ICE_TX_TSTAMP_WORK_PENDING if there are any outstanding Tx
 * timestamps that need processing, and ICE_TX_TSTAMP_WORK_DONE otherwise.
 */
enum ice_tx_tstamp_work ice_ptp_process_ts(struct ice_pf *pf)
{
	return ice_ptp_tx_tstamp(&pf->ptp.port.tx);
}

static void ice_ptp_periodic_work(struct kthread_work *work)
{
	struct ice_ptp *ptp = container_of(work, struct ice_ptp, work.work);
	struct ice_pf *pf = container_of(ptp, struct ice_pf, ptp);
	int err;

	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return;

	err = ice_ptp_update_cached_phctime(pf);

	/* Run twice a second or reschedule if phc update failed */
	kthread_queue_delayed_work(ptp->kworker, &ptp->work,
				   msecs_to_jiffies(err ? 10 : 500));
}

/**
 * ice_ptp_reset - Initialize PTP hardware clock support after reset
 * @pf: Board private structure
 */
void ice_ptp_reset(struct ice_pf *pf)
{
	struct ice_ptp *ptp = &pf->ptp;
	struct ice_hw *hw = &pf->hw;
	struct timespec64 ts;
	int err, itr = 1;
	u64 time_diff;

	if (test_bit(ICE_PFR_REQ, pf->state))
		goto pfr;

	if (!hw->func_caps.ts_func_info.src_tmr_owned)
		goto reset_ts;

	err = ice_ptp_init_phc(hw);
	if (err)
		goto err;

	/* Acquire the global hardware lock */
	if (!ice_ptp_lock(hw)) {
		err = -EBUSY;
		goto err;
	}

	/* Write the increment time value to PHY and LAN */
	err = ice_ptp_write_incval(hw, ice_base_incval(pf));
	if (err) {
		ice_ptp_unlock(hw);
		goto err;
	}

	/* Write the initial Time value to PHY and LAN using the cached PHC
	 * time before the reset and time difference between stopping and
	 * starting the clock.
	 */
	if (ptp->cached_phc_time) {
		time_diff = ktime_get_real_ns() - ptp->reset_time;
		ts = ns_to_timespec64(ptp->cached_phc_time + time_diff);
	} else {
		ts = ktime_to_timespec64(ktime_get_real());
	}
	err = ice_ptp_write_init(pf, &ts);
	if (err) {
		ice_ptp_unlock(hw);
		goto err;
	}

	/* Release the global hardware lock */
	ice_ptp_unlock(hw);

	if (!ice_is_e810(hw)) {
		/* Enable quad interrupts */
		err = ice_ptp_tx_ena_intr(pf, true, itr);
		if (err)
			goto err;
	}

reset_ts:
	/* Restart the PHY timestamping block */
	ice_ptp_reset_phy_timestamping(pf);

pfr:
	/* Init Tx structures */
	if (ice_is_e810(&pf->hw)) {
		err = ice_ptp_init_tx_e810(pf, &ptp->port.tx);
	} else {
		kthread_init_delayed_work(&ptp->port.ov_work,
					  ice_ptp_wait_for_offsets);
		err = ice_ptp_init_tx_e822(pf, &ptp->port.tx,
					   ptp->port.port_num);
	}
	if (err)
		goto err;

	set_bit(ICE_FLAG_PTP, pf->flags);

	/* Start periodic work going */
	kthread_queue_delayed_work(ptp->kworker, &ptp->work, 0);

	dev_info(ice_pf_to_dev(pf), "PTP reset successful\n");
	return;

err:
	dev_err(ice_pf_to_dev(pf), "PTP reset failed %d\n", err);
}

/**
 * ice_ptp_prepare_for_reset - Prepare PTP for reset
 * @pf: Board private structure
 */
void ice_ptp_prepare_for_reset(struct ice_pf *pf)
{
	struct ice_ptp *ptp = &pf->ptp;
	u8 src_tmr;

	clear_bit(ICE_FLAG_PTP, pf->flags);

	/* Disable timestamping for both Tx and Rx */
	ice_ptp_cfg_timestamp(pf, false);

	kthread_cancel_delayed_work_sync(&ptp->work);

	if (test_bit(ICE_PFR_REQ, pf->state))
		return;

	ice_ptp_release_tx_tracker(pf, &pf->ptp.port.tx);

	/* Disable periodic outputs */
	ice_ptp_disable_all_clkout(pf);

	src_tmr = ice_get_ptp_src_clock_index(&pf->hw);

	/* Disable source clock */
	wr32(&pf->hw, GLTSYN_ENA(src_tmr), (u32)~GLTSYN_ENA_TSYN_ENA_M);

	/* Acquire PHC and system timer to restore after reset */
	ptp->reset_time = ktime_get_real_ns();
}

/**
 * ice_ptp_init_owner - Initialize PTP_1588_CLOCK device
 * @pf: Board private structure
 *
 * Setup and initialize a PTP clock device that represents the device hardware
 * clock. Save the clock index for other functions connected to the same
 * hardware resource.
 */
static int ice_ptp_init_owner(struct ice_pf *pf)
{
	struct ice_hw *hw = &pf->hw;
	struct timespec64 ts;
	int err, itr = 1;

	err = ice_ptp_init_phc(hw);
	if (err) {
		dev_err(ice_pf_to_dev(pf), "Failed to initialize PHC, err %d\n",
			err);
		return err;
	}

	/* Acquire the global hardware lock */
	if (!ice_ptp_lock(hw)) {
		err = -EBUSY;
		goto err_exit;
	}

	/* Write the increment time value to PHY and LAN */
	err = ice_ptp_write_incval(hw, ice_base_incval(pf));
	if (err) {
		ice_ptp_unlock(hw);
		goto err_exit;
	}

	ts = ktime_to_timespec64(ktime_get_real());
	/* Write the initial Time value to PHY and LAN */
	err = ice_ptp_write_init(pf, &ts);
	if (err) {
		ice_ptp_unlock(hw);
		goto err_exit;
	}

	/* Release the global hardware lock */
	ice_ptp_unlock(hw);

	if (!ice_is_e810(hw)) {
		/* Enable quad interrupts */
		err = ice_ptp_tx_ena_intr(pf, true, itr);
		if (err)
			goto err_exit;
	}

	/* Ensure we have a clock device */
	err = ice_ptp_create_clock(pf);
	if (err)
		goto err_clk;

	/* Store the PTP clock index for other PFs */
	ice_set_ptp_clock_index(pf);

	return 0;

err_clk:
	pf->ptp.clock = NULL;
err_exit:
	return err;
}

/**
 * ice_ptp_init_work - Initialize PTP work threads
 * @pf: Board private structure
 * @ptp: PF PTP structure
 */
static int ice_ptp_init_work(struct ice_pf *pf, struct ice_ptp *ptp)
{
	struct kthread_worker *kworker;

	/* Initialize work functions */
	kthread_init_delayed_work(&ptp->work, ice_ptp_periodic_work);

	/* Allocate a kworker for handling work required for the ports
	 * connected to the PTP hardware clock.
	 */
	kworker = kthread_create_worker(0, "ice-ptp-%s",
					dev_name(ice_pf_to_dev(pf)));
	if (IS_ERR(kworker))
		return PTR_ERR(kworker);

	ptp->kworker = kworker;

	/* Start periodic work going */
	kthread_queue_delayed_work(ptp->kworker, &ptp->work, 0);

	return 0;
}

/**
 * ice_ptp_init_port - Initialize PTP port structure
 * @pf: Board private structure
 * @ptp_port: PTP port structure
 */
static int ice_ptp_init_port(struct ice_pf *pf, struct ice_ptp_port *ptp_port)
{
	mutex_init(&ptp_port->ps_lock);

	if (ice_is_e810(&pf->hw))
		return ice_ptp_init_tx_e810(pf, &ptp_port->tx);

	kthread_init_delayed_work(&ptp_port->ov_work,
				  ice_ptp_wait_for_offsets);
	return ice_ptp_init_tx_e822(pf, &ptp_port->tx, ptp_port->port_num);
}

/**
 * ice_ptp_init - Initialize PTP hardware clock support
 * @pf: Board private structure
 *
 * Set up the device for interacting with the PTP hardware clock for all
 * functions, both the function that owns the clock hardware, and the
 * functions connected to the clock hardware.
 *
 * The clock owner will allocate and register a ptp_clock with the
 * PTP_1588_CLOCK infrastructure. All functions allocate a kthread and work
 * items used for asynchronous work such as Tx timestamps and periodic work.
 */
void ice_ptp_init(struct ice_pf *pf)
{
	struct ice_ptp *ptp = &pf->ptp;
	struct ice_hw *hw = &pf->hw;
	int err;

	/* If this function owns the clock hardware, it must allocate and
	 * configure the PTP clock device to represent it.
	 */
	if (hw->func_caps.ts_func_info.src_tmr_owned) {
		err = ice_ptp_init_owner(pf);
		if (err)
			goto err;
	}

	ptp->port.port_num = hw->pf_id;
	err = ice_ptp_init_port(pf, &ptp->port);
	if (err)
		goto err;

	/* Start the PHY timestamping block */
	ice_ptp_reset_phy_timestamping(pf);

	set_bit(ICE_FLAG_PTP, pf->flags);
	err = ice_ptp_init_work(pf, ptp);
	if (err)
		goto err;

	dev_info(ice_pf_to_dev(pf), "PTP init successful\n");
	return;

err:
	/* If we registered a PTP clock, release it */
	if (pf->ptp.clock) {
		ptp_clock_unregister(ptp->clock);
		pf->ptp.clock = NULL;
	}
	clear_bit(ICE_FLAG_PTP, pf->flags);
	dev_err(ice_pf_to_dev(pf), "PTP failed %d\n", err);
}

/**
 * ice_ptp_release - Disable the driver/HW support and unregister the clock
 * @pf: Board private structure
 *
 * This function handles the cleanup work required from the initialization by
 * clearing out the important information and unregistering the clock
 */
void ice_ptp_release(struct ice_pf *pf)
{
	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return;

	/* Disable timestamping for both Tx and Rx */
	ice_ptp_cfg_timestamp(pf, false);

	ice_ptp_release_tx_tracker(pf, &pf->ptp.port.tx);

	clear_bit(ICE_FLAG_PTP, pf->flags);

	kthread_cancel_delayed_work_sync(&pf->ptp.work);

	ice_ptp_port_phy_stop(&pf->ptp.port);
	mutex_destroy(&pf->ptp.port.ps_lock);
	if (pf->ptp.kworker) {
		kthread_destroy_worker(pf->ptp.kworker);
		pf->ptp.kworker = NULL;
	}

	if (!pf->ptp.clock)
		return;

	/* Disable periodic outputs */
	ice_ptp_disable_all_clkout(pf);

	ice_clear_ptp_clock_index(pf);
	ptp_clock_unregister(pf->ptp.clock);
	pf->ptp.clock = NULL;

	dev_info(ice_pf_to_dev(pf), "Removed PTP clock\n");
}
