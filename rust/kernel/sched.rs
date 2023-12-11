// SPDX-License-Identifier: GPL-2.0

//! sched
//!
//! C header: [`include/linux/sched.h`](../../../../include/linux/sched.h)

use crate::{bindings, c_types, types};

pub fn schedule() {
    unsafe {
        bindings::schedule();
    }
}

pub fn sched_setscheduler(
    arg1: *mut bindings::task_struct,
    arg2: c_types::c_int,
    arg3: *const types::SchedParam,
) -> c_types::c_int {
    unsafe { bindings::sched_setscheduler(arg1, arg2, arg3 as *const bindings::sched_param) }
}
