// SPDX-License-Identifier: GPL-2.0

//! waitqueue
//!
//! C header: [`include/linux/wait.h`](../../../../include/linux/wait.h)

use crate::{
    bindings,
    c_types::*,
};
use core::ptr;

extern "C" {
    fn rust_helper_add_wait_queue(wq_head: *mut bindings::wait_queue_head, wq_entry: *mut bindings::wait_queue_entry);
    fn rust_helper_wait_event_interruptible(wq_head: *mut bindings::wait_queue_head, condition: bool) -> i32;
    #[allow(improper_ctypes)]
    fn __init_waitqueue_head(
        wq_head: *mut bindings::wait_queue_head,
        name: *const c_char,
        arg1: *mut bindings::lock_class_key,
    );
    fn rust_helper_wq_has_sleeper(wq_head: *mut bindings::wait_queue_head) -> bool;
    fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
    fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
}

#[derive(Default)]
pub struct LockClassKey {
    lock_class_key: bindings::lock_class_key,
}

#[derive(Default)]
pub struct WaitQueueEntry {
    pub wait_queue_entry: bindings::wait_queue_entry_t,
}

impl WaitQueueEntry {
    pub fn init_wait_entry(&mut self, flags: c_int) {
        unsafe {
            bindings::init_wait_entry(
                &mut self.wait_queue_entry as *mut bindings::wait_queue_entry,
                flags,
            );
        }
    }
}

#[derive(Default)]
pub struct WaitQueueHead {
    pub wait_queue_head: bindings::wait_queue_head_t,
}

impl WaitQueueHead {
    pub fn new() -> Self {
        WaitQueueHead {
            wait_queue_head: bindings::wait_queue_head_t {
                lock: bindings::spinlock_t {
                    _bindgen_opaque_blob: 0,
                },
                head: bindings::list_head {
                    next: ptr::null_mut(),
                    prev: ptr::null_mut(),
                },
            },
        }
    }

    pub fn raw_spin_lock_irqsave(&mut self) -> u64 {
        unsafe {
            rust_helper_raw_spin_lock_irqsave(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::hard_spinlock_t,
            )
        }
    }

    pub fn raw_spin_unlock_irqrestore(&mut self, flags: u64) {
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::hard_spinlock_t,
                flags,
            );
        }
    }

    pub fn add_wait_queue(&mut self, wq_entry: &mut WaitQueueEntry) {
        let ptr_wq_entry = &mut wq_entry.wait_queue_entry as *mut bindings::wait_queue_entry_t;
        unsafe {
            rust_helper_add_wait_queue(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                ptr_wq_entry,
            );
        }
    }

    pub fn wait_event_interruptible(&mut self, condition: bool) -> i32 {
        unsafe {
            rust_helper_wait_event_interruptible(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                condition,
            )
        }
    }

    pub fn init_waitqueue_head(&mut self, name: *const c_char, arg1: &mut LockClassKey) {
        let ptr_arg1 = &mut arg1.lock_class_key as *mut bindings::lock_class_key;
        unsafe {
            __init_waitqueue_head(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                name,
                ptr_arg1,
            );
        }
    }

    pub fn wake_up(&mut self, mode: c_uint, nr: c_int, key: *mut c_void) {
        unsafe {
            bindings::__wake_up(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                mode,
                nr,
                key,
            );
        }
    }

    pub fn wq_has_sleeper(&mut self) -> bool {
        unsafe {
            rust_helper_wq_has_sleeper(&mut self.wait_queue_head as *mut bindings::wait_queue_head)
        }
    }
}
