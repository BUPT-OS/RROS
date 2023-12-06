// SPDX-License-Identifier: GPL-2.0

//! ktime
//!
//! C header: [`include/linux/ktime.h`](../../../../include/linux/ktime.h)
use crate::bindings;
pub type KtimeT = i64;

extern "C" {
    fn rust_helper_ktime_sub(lhs: KtimeT, rhs: KtimeT) -> KtimeT;
    fn rust_helper_ktime_add_ns(kt: KtimeT, nsval: u64) -> KtimeT;
    fn rust_helper_ktime_add(kt: KtimeT, nsval: KtimeT) -> KtimeT;
    fn rust_helper_ktime_set(secs: i64, nsecs: usize) -> KtimeT;
    fn rust_helper_ktime_divns(kt: KtimeT, div: i64) -> i64;
    fn rust_helper_ktime_compare(cmp1: KtimeT, cmp2: KtimeT) -> KtimeT;
}
pub fn ktime_get() -> KtimeT {
    unsafe { bindings::ktime_get() as KtimeT }
}
pub fn ktime_sub(lhs: KtimeT, rhs: KtimeT) -> KtimeT {
    unsafe { rust_helper_ktime_sub(lhs, rhs) }
}

pub fn ktime_add_ns(kt: KtimeT, nsval: u64) -> KtimeT {
    unsafe { rust_helper_ktime_add_ns(kt, nsval) }
}

pub fn ktime_add(kt: KtimeT, nsval: KtimeT) -> KtimeT {
    unsafe { rust_helper_ktime_add(kt, nsval) }
}

pub fn ktime_to_ns(kt: KtimeT) -> i64 {
    kt as i64
}

pub fn ktime_set(secs: i64, nsecs: usize) -> KtimeT {
    unsafe { rust_helper_ktime_set(secs, nsecs) }
}

pub fn ktime_divns(kt: KtimeT, div: i64) -> i64 {
    unsafe { rust_helper_ktime_divns(kt, div) }
}

pub fn ktime_compare(cmp1: KtimeT, cmp2: KtimeT) -> KtimeT {
    unsafe { rust_helper_ktime_compare(cmp1, cmp2) }
}
