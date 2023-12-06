// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/vmalloc.h`](../../../../include/linux/vmalloc.h)

use crate::{
    bindings::{self, gfp_t, GFP_KERNEL},
    c_types,
};

extern "C" {
    fn rust_helper_kzalloc(size: usize, flags: gfp_t) -> *mut c_types::c_void;
}

pub fn c_vmalloc(size: c_types::c_ulong) -> Option<*mut c_types::c_void> {
    let ptr = unsafe { bindings::vmalloc(size) };

    if ptr.is_null() {
        return None;
    }

    Some(ptr)
}

pub fn c_vfree(ptr: *const c_types::c_void) {
    unsafe {
        bindings::vfree(ptr);
    }
}

pub fn c_kzalloc(size: c_types::c_ulong) -> Option<*mut c_types::c_void> {
    let ptr = unsafe { rust_helper_kzalloc(size as usize, GFP_KERNEL) };

    if ptr.is_null() {
        return None;
    }

    Some(ptr)
}

pub fn c_kzfree(ptr: *const c_types::c_void) {
    unsafe {
        bindings::kfree(ptr);
    }
}
