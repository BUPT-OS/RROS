// SPDX-License-Identifier: GPL-2.0

//! if_packet
//!
//! C header: [`include/linux/if_packet.h`](../../../../include/linux/if_packet.h)

use crate::bindings;
use core::cell::UnsafeCell;

#[derive(Default)]
#[repr(transparent)]
pub struct SockaddrLL(pub(crate) UnsafeCell<bindings::sockaddr_ll>);

impl SockaddrLL {
    pub fn get_mut(&mut self) -> &mut bindings::sockaddr_ll {
        self.0.get_mut()
    }
}
