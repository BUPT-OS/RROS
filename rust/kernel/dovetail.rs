// SPDX-License-Identifier: GPL-2.0

//! dovetail
//!
//! C header: [`include/linux/dovetail.h`](../../../../include/linux/dovetail.h)
use crate::{bindings, prelude::*, c_types::*};

use core::ptr;

extern "C" {
    #[allow(improper_ctypes)]
    fn rust_helper_dovetail_current_state() -> *mut bindings::oob_thread_state;
    fn rust_helper_dovetail_leave_oob();
    fn rust_helper_dovetail_request_ucall(task: *mut bindings::task_struct);
    fn rust_helper_dovetail_mm_state() -> *mut bindings::oob_mm_state;
}

/// The `dovetail_start` function is a wrapper around the `bindings::dovetail_start` function from the kernel bindings. It starts the Dovetail interface in the kernel.
///
/// This function does not take any arguments. It calls the `bindings::dovetail_start` function and checks the return value. If the return value is 0, it returns `Ok(0)`. Otherwise, it returns `Err(Error::EINVAL)`.
///
/// This function is unsafe because it calls an unsafe function from the kernel bindings.
pub fn dovetail_start() -> Result<usize> {
    let res = unsafe { bindings::dovetail_start() };
    if res == 0 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}

pub struct OobThreadState {
    pub(crate) ptr: *mut bindings::oob_thread_state,
}

impl OobThreadState {
    pub(crate) unsafe fn from_ptr(ptr: *mut bindings::oob_thread_state) -> Self {
        Self { ptr }
    }

    pub fn preempt_count(&self) -> i32 {
        unsafe { (*(self.ptr)).preempt_count }
    }

    pub fn thread(&self) -> *mut c_void {
        unsafe { (*(self.ptr)).thread }
    }

    pub fn set_thread(&self, curr: *mut c_void) {
        unsafe { (*(self.ptr)).thread = curr; }
    }
}

pub fn dovetail_current_state() -> OobThreadState {
    let ptr = unsafe { rust_helper_dovetail_current_state() };
    unsafe { OobThreadState::from_ptr(ptr) }
}

#[repr(transparent)]
pub struct DovetailAltschedContext(pub bindings::dovetail_altsched_context);

impl DovetailAltschedContext {
    pub fn new() -> Self {
        Self(bindings::dovetail_altsched_context::default())
    }

    pub fn dovetail_init_altsched(&mut self) {
        unsafe {
            bindings::dovetail_init_altsched(
                &mut self.0 as *mut bindings::dovetail_altsched_context,
            );
        }
    }
}

pub fn dovetail_context_switch(
    out: &mut DovetailAltschedContext,
    in_: &mut DovetailAltschedContext,
    leave_inband: bool,
) -> bool {
    let ptr_out = &mut out.0 as *mut bindings::dovetail_altsched_context;
    let ptr_in_ = &mut in_.0 as *mut bindings::dovetail_altsched_context;
    unsafe { bindings::dovetail_context_switch(ptr_out, ptr_in_, leave_inband) }
}

pub fn dovetail_resume_inband() {
    unsafe {
        bindings::dovetail_resume_inband();
    }
}

pub fn dovetail_start_altsched() {
    unsafe {
        bindings::dovetail_start_altsched();
    }
}

pub fn dovetail_leave_inband() -> c_int {
    unsafe { bindings::dovetail_leave_inband() }
}

pub fn dovetail_stop_altsched() {
    unsafe {
        bindings::dovetail_stop_altsched();
    }
}

pub fn dovetail_leave_oob() {
    unsafe {
        rust_helper_dovetail_leave_oob();
    }
}

pub fn dovetail_request_ucall(ptr: *mut bindings::task_struct) {
    unsafe {
        rust_helper_dovetail_request_ucall(ptr);
    }
}

#[derive(Copy, Clone)]
pub struct OobMmState {
    pub ptr: *mut bindings::oob_mm_state,
}

impl OobMmState {
    pub fn new() -> Self {
        Self {
            ptr: ptr::null_mut(),
        }
    }

    pub fn is_null(&self) -> bool {
        self.ptr.is_null()
    }
}

pub fn dovetail_mm_state() -> OobMmState {
    unsafe {
        OobMmState {
            ptr: rust_helper_dovetail_mm_state(),
        }
    }
}
