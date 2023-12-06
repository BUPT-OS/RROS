// SPDX-License-Identifier: GPL-2.0

//! ktime_t - nanosecond-resolution time format.
//!
//! C header: [`include/linux/percpu.h`](../../../../include/linux/percpu.h)

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
};

pub fn alloc_per_cpu(size: usize, align: usize) -> *mut u8 {
    unsafe {
        return bindings::__alloc_percpu(size, align) as *mut u8;
    }
}

pub fn free_per_cpu(pdata: *mut u8) {
    unsafe {
        bindings::free_percpu(pdata as *mut c_types::c_void);
    }
}
