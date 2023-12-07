// SPDX-License-Identifier: GPL-2.0

//! kernel
//!
//! C header: [`include/linux/kernel.h`](../../../../include/linux/kernel.h)

use crate::{
    bindings,
    prelude::*,
    Result,
    c_types::*,
};

// FIXME: how to wrapper `...` in parameters
#[inline]
pub fn _kasprintf_1(gfp: bindings::gfp_t, fmt: *const c_char, arg1: *const c_char) -> *mut c_char {
    unsafe { bindings::kasprintf(gfp, fmt, arg1) }
}

#[inline]
pub fn _kasprintf_2(gfp: bindings::gfp_t, fmt: *const c_char, arg1: *const c_char,arg2: *const c_char) -> *mut c_char {
    unsafe { bindings::kasprintf(gfp, fmt, arg1,arg2) }
}

// unused macro, refactor later
#[macro_export]
macro_rules! kasprintf {
    ($gfp:expr,$fmt:expr, $arg1:expr) => {
        use kernel::kernelh::_kasprintf_1;
        unsafe {
            _kasprintf_1($gfp, $fmt, $arg1)
        }
    };
    ($gfp:expr,$fmt:expr, $arg1:expr,$arg2:expr) => {
        use kernel::kernelh::_kasprintf_2;
        unsafe {
            _kasprintf_2($gfp, $fmt, $arg1,$arg2)
        }
    };
}

pub fn do_exit(error_code: c_long) {
    unsafe { bindings::do_exit(error_code); }
}