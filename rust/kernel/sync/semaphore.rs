// SPDX-License-Identifier: GPL-2.0

//! semaphore
//!
//! C header: [`include/linux/semaphore.h`](../../../../include/linux/semaphore.h)

// TODO: To be refactored to rust-style once updated to the latest version of the Linux.
use crate::{bindings, error::Error, Result};

extern "C" {
    fn rust_helper_sema_init(sem: *mut bindings::semaphore, val: i32);
}

/// The `Semaphore` struct is a wrapper around the `bindings::semaphore` struct from the kernel bindings. It represents a semaphore object in the kernel.
pub struct Semaphore(bindings::semaphore);

impl Semaphore {
    /// The `new` method is a constructor for `Semaphore`. It creates a new `Semaphore` with a default `bindings::semaphore`.
    pub fn new() -> Self {
        let sem = bindings::semaphore::default();
        Self(sem)
    }

    /// The `init` method initializes the semaphore with a given value. It does this by calling the unsafe `rust_helper_sema_init` function with a pointer to the underlying `bindings::semaphore` and the initial value.
    pub fn init(&mut self, val: i32) {
        unsafe {
            rust_helper_sema_init(&mut self.0 as *mut bindings::semaphore, val);
        }
    }

    /// The `down` method decreases the semaphore count, blocking if the count is zero. It calls the unsafe `bindings::down` function with a pointer to the underlying `bindings::semaphore`.
    pub fn down(&mut self) {
        unsafe {
            bindings::down(&mut self.0 as *mut bindings::semaphore);
        }
    }

    /// The `down_interruptible` method is similar to `down`, but it is interruptible.
    pub fn down_interruptible(&mut self) -> Result<i32> {
        let ret = unsafe { bindings::down_interruptible(&mut self.0 as *mut bindings::semaphore) };
        if ret == 0 {
            Ok(ret)
        } else {
            Err(Error::EINTR)
        }
    }

    /// The `up` method increases the semaphore count, signaling that the resource is available. It calls the unsafe `bindings::up` function with a pointer to the underlying `bindings::semaphore`.
    pub fn up(&mut self) {
        unsafe {
            bindings::up(&mut self.0 as *mut bindings::semaphore);
        }
    }
}
