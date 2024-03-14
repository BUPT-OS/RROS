/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Intel Corporation. */

#ifndef _ICE_VF_MBX_H_
#define _ICE_VF_MBX_H_

#include "ice_type.h"
#include "ice_controlq.h"

/* Defining the mailbox message threshold as 63 asynchronous
 * pending messages. Normal VF functionality does not require
 * sending more than 63 asynchronous pending message.
 */
#define ICE_ASYNC_VF_MSG_THRESHOLD	63

#ifdef CONFIG_PCI_IOV
int
ice_aq_send_msg_to_vf(struct ice_hw *hw, u16 vfid, u32 v_opcode, u32 v_retval,
		      u8 *msg, u16 msglen, struct ice_sq_cd *cd);

u32 ice_conv_link_speed_to_virtchnl(bool adv_link_support, u16 link_speed);
int
ice_mbx_vf_state_handler(struct ice_hw *hw, struct ice_mbx_data *mbx_data,
			 struct ice_mbx_vf_info *vf_info, bool *report_malvf);
void ice_mbx_clear_malvf(struct ice_mbx_vf_info *vf_info);
void ice_mbx_init_vf_info(struct ice_hw *hw, struct ice_mbx_vf_info *vf_info);
void ice_mbx_init_snapshot(struct ice_hw *hw);
#else /* CONFIG_PCI_IOV */
static inline int
ice_aq_send_msg_to_vf(struct ice_hw __always_unused *hw,
		      u16 __always_unused vfid, u32 __always_unused v_opcode,
		      u32 __always_unused v_retval, u8 __always_unused *msg,
		      u16 __always_unused msglen,
		      struct ice_sq_cd __always_unused *cd)
{
	return 0;
}

static inline u32
ice_conv_link_speed_to_virtchnl(bool __always_unused adv_link_support,
				u16 __always_unused link_speed)
{
	return 0;
}

static inline void ice_mbx_init_snapshot(struct ice_hw *hw)
{
}

#endif /* CONFIG_PCI_IOV */
#endif /* _ICE_VF_MBX_H_ */
