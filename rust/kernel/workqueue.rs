// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/workqueue.h`](../../../../include/linux/workqueue.h)

use crate::{
    bindings, c_str,
    Opaque,
    prelude::*,
    Result,
};

use core::{
    fmt, ops::Deref, ptr::NonNull,
};

pub struct Queue(Opaque<bindings::workqueue_struct>);

unsafe impl Sync for Queue {}

impl Queue {
    /// Tries to allocate a new work queue.
    ///
    /// Callers should first consider using one of the existing ones (e.g. [`system`]) before
    /// deciding to create a new one.
    pub fn try_new(name: fmt::Arguments<'_>) -> Result<BoxedQueue> {
        // SAFETY: We use a format string that requires an `fmt::Arguments` pointer as the first
        // and only argument.
        let ptr = unsafe {
            bindings::alloc_workqueue(
                c_str!("%pA").as_char_ptr(),
                0,
                0,
                &name as *const _ as *const core::ffi::c_void,
            )
        };
        if ptr.is_null() {
            return Err(Error::ENOMEM);
        }

        // SAFETY: `ptr` was just allocated and checked above, so it non-null and valid. Plus, it
        // isn't touched after the call below, so ownership is transferred.
        Ok(unsafe { BoxedQueue::new(ptr) })
    }

    pub fn get_ptr(&self) -> *mut bindings::workqueue_struct {
        self.0.get()
    } 
}

#[repr(transparent)]
pub struct Work(Opaque<bindings::work_struct>);

impl Work {
    /// Creates a new instance of [`Work`].
    ///
    /// # Safety
    ///
    /// Callers must call either [`Work::init`] or [`Work::init_with_adapter`] before the work item
    /// can be used.
    pub unsafe fn new() -> Self {
        Self(Opaque::uninit())
    }
}


/// A boxed owned workqueue.
///
/// # Invariants
///
/// `ptr` is owned by this instance of [`BoxedQueue`], so it's always valid.
pub struct BoxedQueue {
    ptr: NonNull<Queue>,
}

impl BoxedQueue {
    /// Creates a new instance of [`BoxedQueue`].
    ///
    /// # Safety
    ///
    /// `ptr` must be non-null and valid. Additionally, ownership must be handed over to new
    /// instance of [`BoxedQueue`].
    unsafe fn new(ptr: *mut bindings::workqueue_struct) -> Self {
        Self {
            // SAFETY: We checked above that `ptr` is non-null.
            ptr: unsafe { NonNull::new_unchecked(ptr.cast()) },
        }
    }
}

impl Deref for BoxedQueue {
    type Target = Queue;

    fn deref(&self) -> &Queue {
        // SAFETY: The type invariants guarantee that `ptr` is always valid.
        unsafe { self.ptr.as_ref() }
    }
}

impl Drop for BoxedQueue {
    fn drop(&mut self) {
        // SAFETY: The type invariants guarantee that `ptr` is always valid.
        unsafe { bindings::destroy_workqueue(self.ptr.as_ref().0.get()) };
    }
}