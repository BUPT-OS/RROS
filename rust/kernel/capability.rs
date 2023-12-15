// SPDX-License-Identifier: GPL-2.0

//! capability
//!
//! C header: [`include/linux/capability.h`](../../../../include/linux/capability.h)

use crate::{
    bindings,
};

extern "C" {
    fn rust_helper_current_cap() -> bindings::kernel_cap_t;
    fn rust_helper_cap_raised(c: bindings::kernel_cap_t, f: i32) -> i32;
    fn rust_helper_cap_raise(c: *mut bindings::kernel_cap_t, flag: i32);
}

// TODO: conditional compilation
#[inline]
#[cfg(CONFIG_MULTIUSER)]
pub fn capable(cap: i32) -> bool {
    unsafe { bindings::capable(cap) }
}

#[inline]
#[cfg(not(CONFIG_MULTIUSER))]
pub fn capable(cap: i32) -> bool {
    true
}

#[repr(transparent)]
pub struct KernelCapStruct {
    pub kernel_cap_struct: bindings::kernel_cap_struct,
}

impl KernelCapStruct {
    pub fn new() -> KernelCapStruct {
        KernelCapStruct {
            kernel_cap_struct: bindings::kernel_cap_struct { cap: [0, 0] },
        }
    }

    pub fn current_cap() -> Self {
        unsafe {
            Self {
                kernel_cap_struct: rust_helper_current_cap(),
            }
        }
    }

    pub fn cap_raise(&mut self, flag: i32) {
        unsafe {
            rust_helper_cap_raise(
                &mut self.kernel_cap_struct as *mut bindings::kernel_cap_t,
                flag,
            );
        }
    }

    pub fn cap_raised(c: KernelCapStruct, flag: i32) -> i32 {
        unsafe { rust_helper_cap_raised(c.kernel_cap_struct, flag) }
    }
}
