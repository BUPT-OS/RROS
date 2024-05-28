// SPDX-License-Identifier: GPL-2.0

//! sched
//!
//! C header: [`include/linux/sched.h`](../../../../include/linux/sched.h)

use crate::{bindings, c_types, types};

/// The `schedule` function is a wrapper around the `bindings::schedule` function from the kernel bindings.
pub fn schedule() {
    unsafe {
        bindings::schedule();
    }
}

/// The `sched_setscheduler` function is a wrapper around the `bindings::sched_setscheduler` function from the kernel bindings.
pub fn sched_setscheduler(
    arg1: *mut bindings::task_struct,
    arg2: c_types::c_int,
    arg3: *const types::SchedParam,
) -> c_types::c_int {
    unsafe { bindings::sched_setscheduler(arg1, arg2, arg3 as *const bindings::sched_param) }
}
