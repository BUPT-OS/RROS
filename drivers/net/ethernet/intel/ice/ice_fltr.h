/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2020, Intel Corporation. */

#ifndef _ICE_FLTR_H_
#define _ICE_FLTR_H_

#include "ice_vlan.h"

void ice_fltr_free_list(struct device *dev, struct list_head *h);
int
ice_fltr_set_vlan_vsi_promisc(struct ice_hw *hw, struct ice_vsi *vsi,
			      u8 promisc_mask);
int
ice_fltr_clear_vlan_vsi_promisc(struct ice_hw *hw, struct ice_vsi *vsi,
				u8 promisc_mask);
int
ice_fltr_clear_vsi_promisc(struct ice_hw *hw, u16 vsi_handle, u8 promisc_mask,
			   u16 vid);
int
ice_fltr_set_vsi_promisc(struct ice_hw *hw, u16 vsi_handle, u8 promisc_mask,
			 u16 vid);
int
ice_fltr_add_mac_to_list(struct ice_vsi *vsi, struct list_head *list,
			 const u8 *mac, enum ice_sw_fwd_act_type action);
int
ice_fltr_add_mac(struct ice_vsi *vsi, const u8 *mac,
		 enum ice_sw_fwd_act_type action);
int
ice_fltr_add_mac_and_broadcast(struct ice_vsi *vsi, const u8 *mac,
			       enum ice_sw_fwd_act_type action);
int ice_fltr_add_mac_list(struct ice_vsi *vsi, struct list_head *list);
int
ice_fltr_remove_mac(struct ice_vsi *vsi, const u8 *mac,
		    enum ice_sw_fwd_act_type action);
int ice_fltr_remove_mac_list(struct ice_vsi *vsi, struct list_head *list);

int ice_fltr_add_vlan(struct ice_vsi *vsi, struct ice_vlan *vlan);
int ice_fltr_remove_vlan(struct ice_vsi *vsi, struct ice_vlan *vlan);

int
ice_fltr_add_eth(struct ice_vsi *vsi, u16 ethertype, u16 flag,
		 enum ice_sw_fwd_act_type action);
int
ice_fltr_remove_eth(struct ice_vsi *vsi, u16 ethertype, u16 flag,
		    enum ice_sw_fwd_act_type action);
void ice_fltr_remove_all(struct ice_vsi *vsi);

int
ice_fltr_update_flags(struct ice_vsi *vsi, u16 rule_id, u16 recipe_id,
		      u32 new_flags);
#endif
