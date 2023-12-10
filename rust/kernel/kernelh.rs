// SPDX-License-Identifier: GPL-2.0

//! kernel
//!
//! C header: [`include/linux/kernel.h`](../../../../include/linux/kernel.h)

use crate::{
    bindings,
    c_types::*,
};

// FIXME: how to wrapper `...` in parameters
/// The `printk` function is a wrapper around the `printk` function from the kernel.
#[inline]
pub fn _kasprintf_1(gfp: bindings::gfp_t, fmt: *const c_char, arg1: *const c_char) -> *mut c_char {
    unsafe { bindings::kasprintf(gfp, fmt, arg1) }
}

/// The `printk` function is a wrapper around the `printk` function from the kernel.
#[inline]
pub fn _kasprintf_2(gfp: bindings::gfp_t, fmt: *const c_char, arg1: *const c_char,arg2: *const c_char) -> *mut c_char {
    unsafe { bindings::kasprintf(gfp, fmt, arg1,arg2) }
}

/// call linux do_exit
pub fn do_exit(error_code: c_long) {
    unsafe { bindings::do_exit(error_code); }
}