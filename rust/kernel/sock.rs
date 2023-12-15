// SPDX-License-Identifier: GPL-2.0

//! sock
//!
//! C header: [`include/linux/sock.h`](../../../../include/linux/sock.h)

use crate::bindings;
use core::cell::UnsafeCell;

#[repr(transparent)]
pub struct Sock(pub(crate) UnsafeCell<bindings::sock>);

impl Sock {
    pub fn get_mut(&mut self) -> &mut bindings::sock {
        self.0.get_mut()
    }
}
