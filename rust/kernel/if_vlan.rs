// SPDX-License-Identifier: GPL-2.0

//! if_vlan
//!
//! C header: [`include/linux/if_vlan.h`](../../../../include/linux/if_vlan.h)

use crate::bindings;
use core::cell::UnsafeCell;

#[repr(transparent)]
pub struct VlanEthhdr(pub(crate) UnsafeCell<bindings::vlan_ethhdr>);

impl VlanEthhdr {
    pub fn get_mut(&mut self) -> &mut bindings::vlan_ethhdr {
        self.0.get_mut()
    }
}
