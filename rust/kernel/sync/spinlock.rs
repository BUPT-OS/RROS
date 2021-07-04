// SPDX-License-Identifier: GPL-2.0

//! A kernel spinlock.
//!
//! This module allows Rust code to use the kernel's [`struct spinlock`].
//!
//! See <https://www.kernel.org/doc/Documentation/locking/spinlocks.txt>.

use super::{Guard, Lock, NeedsLockClass};
use crate::str::CStr;
use crate::{bindings, c_types};
use core::{cell::UnsafeCell, marker::PhantomPinned, pin::Pin};

extern "C" {
    #[allow(improper_ctypes)]
    fn rust_helper_spin_lock_init(
        lock: *mut bindings::spinlock_t,
        name: *const c_types::c_char,
        key: *mut bindings::lock_class_key,
    );
    fn rust_helper_spin_lock(lock: *mut bindings::spinlock);
    fn rust_helper_spin_unlock(lock: *mut bindings::spinlock);
}

/// Safely initialises a [`SpinLock`] with the given name, generating a new lock class.
#[macro_export]
macro_rules! spinlock_init {
    ($spinlock:expr, $name:literal) => {
        $crate::init_with_lockdep!($spinlock, $name)
    };
}

/// Exposes the kernel's [`spinlock_t`]. When multiple CPUs attempt to lock the same spinlock, only
/// one at a time is allowed to progress, the others will block (spinning) until the spinlock is
/// unlocked, at which point another CPU will be allowed to make progress.
///
/// A [`SpinLock`] must first be initialised with a call to [`SpinLock::init`] before it can be
/// used. The [`spinlock_init`] macro is provided to automatically assign a new lock class to a
/// spinlock instance.
///
/// [`SpinLock`] does not manage the interrupt state, so it can be used in only two cases: (a) when
/// the caller knows that interrupts are disabled, or (b) when callers never use it in interrupt
/// handlers (in which case it is ok for interrupts to be enabled).
///
/// [`spinlock_t`]: ../../../include/linux/spinlock.h
pub struct SpinLock<T: ?Sized> {
    spin_lock: UnsafeCell<bindings::spinlock>,

    /// Spinlocks are architecture-defined. So we conservatively require them to be pinned in case
    /// some architecture uses self-references now or in the future.
    _pin: PhantomPinned,

    data: UnsafeCell<T>,
}

// SAFETY: `SpinLock` can be transferred across thread boundaries iff the data it protects can.
unsafe impl<T: ?Sized + Send> Send for SpinLock<T> {}

// SAFETY: `SpinLock` serialises the interior mutability it provides, so it is `Sync` as long as the
// data it protects is `Send`.
unsafe impl<T: ?Sized + Send> Sync for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// Constructs a new spinlock.
    ///
    /// # Safety
    ///
    /// The caller must call [`SpinLock::init`] before using the spinlock.
    pub unsafe fn new(t: T) -> Self {
        Self {
            spin_lock: UnsafeCell::new(bindings::spinlock::default()),
            data: UnsafeCell::new(t),
            _pin: PhantomPinned,
        }
    }
}

impl<T: ?Sized> SpinLock<T> {
    /// Locks the spinlock and gives the caller access to the data protected by it. Only one thread
    /// at a time is allowed to access the protected data.
    pub fn lock(&self) -> Guard<'_, Self> {
        self.lock_noguard();
        // SAFETY: The spinlock was just acquired.
        unsafe { Guard::new(self) }
    }
}

impl<T: ?Sized> NeedsLockClass for SpinLock<T> {
    unsafe fn init(self: Pin<&mut Self>, name: &'static CStr, key: *mut bindings::lock_class_key) {
        unsafe { rust_helper_spin_lock_init(self.spin_lock.get(), name.as_char_ptr(), key) };
    }
}

impl<T: ?Sized> Lock for SpinLock<T> {
    type Inner = T;

    fn lock_noguard(&self) {
        // SAFETY: `spin_lock` points to valid memory.
        unsafe { rust_helper_spin_lock(self.spin_lock.get()) };
    }

    unsafe fn unlock(&self) {
        unsafe { rust_helper_spin_unlock(self.spin_lock.get()) };
    }

    fn locked_data(&self) -> &UnsafeCell<T> {
        &self.data
    }
}
