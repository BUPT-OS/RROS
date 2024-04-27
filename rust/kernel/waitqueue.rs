// SPDX-License-Identifier: GPL-2.0

//! waitqueue
//!
//! C header: [`include/linux/wait.h`](../../../../include/linux/wait.h)

use crate::{bindings, c_types::*};
use core::ptr;

extern "C" {
    fn rust_helper_add_wait_queue(
        wq_head: *mut bindings::wait_queue_head,
        wq_entry: *mut bindings::wait_queue_entry,
    );
    fn rust_helper_wait_event_interruptible(
        wq_head: *mut bindings::wait_queue_head,
        condition: bool,
    ) -> i32;
    fn rust_helper_init_waitqueue_head(wq: *mut bindings::wait_queue_head);
    #[allow(improper_ctypes)]
    fn __init_waitqueue_head(
        wq_head: *mut bindings::wait_queue_head,
        name: *const c_char,
        arg1: *mut bindings::lock_class_key,
    );
    fn rust_helper_spin_lock_irqsave(lock: *mut bindings::spinlock_t) -> u64;
    fn rust_helper_spin_unlock_irqrestore(lock: *mut bindings::spinlock_t, flags: u64);
    fn rust_helper_wq_has_sleeper(wq_head: *mut bindings::wait_queue_head) -> bool;
    fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
    fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
    fn rust_helper_waitqueue_active(wq: *mut bindings::wait_queue_head) -> i32;
    fn rust_helper_list_empty(list: *const bindings::list_head) -> bool;
    fn rust_helper_list_del(list: *mut bindings::list_head);
}

/// The `LockClassKey` struct wraps a `bindings::lock_class_key` struct from the kernel bindings.
#[derive(Default)]
pub struct LockClassKey {
    lock_class_key: bindings::lock_class_key,
}

/// The `WaitQueueEntry` struct wraps a `bindings::wait_queue_entry_t` struct from the kernel bindings.
#[derive(Default)]
#[repr(transparent)]
pub struct WaitQueueEntry {
    wait_queue_entry: bindings::wait_queue_entry_t,
}

impl WaitQueueEntry {
    /// A wrapper around the `bindings::init_wait_entry` function from the kernel bindings.
    pub fn init_wait_entry(&mut self, flags: c_int) {
        unsafe {
            bindings::init_wait_entry(
                &mut self.wait_queue_entry as *mut bindings::wait_queue_entry,
                flags,
            );
        }
    }

    /// Call `list_empty` from the rust_helper.
    pub fn list_empty(&mut self) -> bool {
        unsafe {
            rust_helper_list_empty(&self.wait_queue_entry.entry as *const bindings::list_head)
        }
    }

    /// Call `list_del` from the rust_helper to delete the entry from the list.
    pub fn list_del(&mut self) {
        unsafe {
            rust_helper_list_del(&mut self.wait_queue_entry.entry as *mut bindings::list_head);
        }
    }
}

/// The `WaitQueueHead` struct wraps a `bindings::wait_queue_head_t` function from the kernel bindings.
#[derive(Default)]
#[repr(transparent)]
pub struct WaitQueueHead {
    wait_queue_head: bindings::wait_queue_head_t,
}

impl WaitQueueHead {
    /// Construct a new default struct.
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

    /// Call `init_waitqueue_head` macro from the rust_helper.
    pub fn init(&mut self) {
        unsafe {
            rust_helper_init_waitqueue_head(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
            );
        }
    }

    /// Call `spin_lock_irqsave` from the rust_helper.
    pub fn spin_lock_irqsave(&mut self) -> u64 {
        unsafe {
            rust_helper_spin_lock_irqsave(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::spinlock_t,
            )
        }
    }

    /// Call `spin_unlock_irqrestore` from the rust_helper.
    pub fn spin_unlock_irqrestore(&mut self, flags: u64) {
        unsafe {
            rust_helper_spin_unlock_irqrestore(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::spinlock_t,
                flags,
            );
        }
    }

    /// Call `raw_spin_lock_irqsave` from the rust_helper.
    pub fn raw_spin_lock_irqsave(&mut self) -> u64 {
        unsafe {
            rust_helper_raw_spin_lock_irqsave(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::hard_spinlock_t,
            )
        }
    }

    /// Call `raw_spin_unlock_irqstore` from the rust_helper.
    pub fn raw_spin_unlock_irqrestore(&mut self, flags: u64) {
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                &mut self.wait_queue_head.lock as *mut _ as *mut bindings::hard_spinlock_t,
                flags,
            );
        }
    }

    /// Call `add_wait_queue` from the rust_helper.
    pub fn add_wait_queue(&mut self, wq_entry: &mut WaitQueueEntry) {
        let ptr_wq_entry = &mut wq_entry.wait_queue_entry as *mut bindings::wait_queue_entry_t;
        unsafe {
            rust_helper_add_wait_queue(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                ptr_wq_entry,
            );
        }
    }

    /// Call `wait_event_interruptible` from the rust_helper.
    pub fn wait_event_interruptible(&mut self, condition: bool) -> i32 {
        unsafe {
            rust_helper_wait_event_interruptible(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
                condition,
            )
        }
    }

    /// Call `__init_waitqueue_head` function from the kernel.
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

    /// A wrapper around the `bindings::__wake_up` from the kernel bindings.
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

    /// Call `wq_has_sleeper` from the rust_helper.
    pub fn wq_has_sleeper(&mut self) -> bool {
        unsafe {
            rust_helper_wq_has_sleeper(&mut self.wait_queue_head as *mut bindings::wait_queue_head)
        }
    }

    /// Call `waitqueue_active` from the rust_helper.
    pub fn waitqueue_active(&mut self) -> bool {
        if unsafe {
            rust_helper_waitqueue_active(
                &mut self.wait_queue_head as *mut bindings::wait_queue_head,
            )
        } == 0
        {
            false
        } else {
            true
        }
    }
}
