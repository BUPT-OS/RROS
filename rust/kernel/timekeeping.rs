// SPDX-License-Identifier: GPL-2.0

//! Kernel timekeeping code and accessor functions.
//!
//! C header: [`include/linux/ktime.h`](../../../../include/linux/timekeeping.h)

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
};

pub fn ktime_get_mono_fast_ns() -> i64 {
    unsafe { bindings::ktime_get_mono_fast_ns() as i64 }
}

pub fn ktime_get_real_fast_ns() -> i64 {
    unsafe { bindings::ktime_get_real_fast_ns() as i64 }
}
