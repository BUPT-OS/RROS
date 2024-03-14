/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2009-2012  Realtek Corporation.*/

#ifndef __RTL92CU_LED_H__
#define __RTL92CU_LED_H__

void rtl92cu_sw_led_on(struct ieee80211_hw *hw, enum rtl_led_pin pin);
void rtl92cu_sw_led_off(struct ieee80211_hw *hw, enum rtl_led_pin pin);
void rtl92cu_led_control(struct ieee80211_hw *hw, enum led_ctl_mode ledaction);

#endif
