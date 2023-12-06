// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/uidgid.h`](../../../../include/linux/uidgid.h)

use crate::{bindings, c_types};

extern "C" {
    fn rust_helper_test_and_set_bit(
        bit: c_types::c_uint,
        p: *mut c_types::c_ulong,
    ) -> c_types::c_int;
}

pub fn bitmap_zalloc(nbits: c_types::c_uint, flags: bindings::gfp_t) -> u64 {
    unsafe { bindings::bitmap_zalloc(nbits, flags) as u64 }
}

pub fn find_first_zero_bit(
    addr: *const c_types::c_ulong,
    size: c_types::c_ulong,
) -> c_types::c_ulong {
    unsafe { bindings::_find_first_zero_bit(addr, size) }
}

pub fn test_and_set_bit(bit: c_types::c_ulong, p: *mut c_types::c_ulong) -> bool {
    let res;
    unsafe {
        res = rust_helper_test_and_set_bit(bit as c_types::c_uint, p);
    }
    if res == 1 {
        return true;
    } else {
        return false;
    }
}
