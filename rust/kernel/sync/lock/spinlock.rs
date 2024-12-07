// SPDX-License-Identifier: GPL-2.0

//! A kernel spinlock.
//!
//! This module allows Rust code to use the kernel's `spinlock_t`.

use crate::bindings;

#[cfg(CONFIG_RROS)]
use super::NeedsLockClass;
#[cfg(CONFIG_RROS)]
use crate::c_types;
#[cfg(CONFIG_RROS)]
use crate::sync::{Guard, Lock};
#[cfg(CONFIG_RROS)]
use crate::types::Opaque;
#[cfg(CONFIG_RROS)]
use core::{cell::UnsafeCell, marker::PhantomPinned, pin::Pin};

use crate::str::CStr;

/// Creates a [`SpinLock`] initialiser with the given name and a newly-created lock class.
///
/// It uses the name if one is given, otherwise it generates one based on the file name and line
/// number.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
#[macro_export]
macro_rules! new_spinlock {
    ($inner:expr $(, $name:literal)? $(,)?) => {
        $crate::sync::SpinLock::new(
            $inner, $crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}

/// A spinlock.
///
/// Exposes the kernel's [`spinlock_t`]. When multiple CPUs attempt to lock the same spinlock, only
/// one at a time is allowed to progress, the others will block (spinning) until the spinlock is
/// unlocked, at which point another CPU will be allowed to make progress.
///
/// Instances of [`SpinLock`] need a lock class and to be pinned. The recommended way to create such
/// instances is with the [`pin_init`](crate::pin_init) and [`new_spinlock`] macros.
///
/// # Examples
///
/// The following example shows how to declare, allocate and initialise a struct (`Example`) that
/// contains an inner struct (`Inner`) that is protected by a spinlock.
///
/// ```
/// use kernel::{init::InPlaceInit, init::PinInit, new_spinlock, pin_init, sync::SpinLock};
///
/// struct Inner {
///     a: u32,
///     b: u32,
/// }
///
/// #[pin_data]
/// struct Example {
///     c: u32,
///     #[pin]
///     d: SpinLock<Inner>,
/// }
///
/// impl Example {
///     fn new() -> impl PinInit<Self> {
///         pin_init!(Self {
///             c: 10,
///             d <- new_spinlock!(Inner { a: 20, b: 30 }),
///         })
///     }
/// }
///
/// // Allocate a boxed `Example`.
/// let e = Box::pin_init(Example::new())?;
/// assert_eq!(e.c, 10);
/// assert_eq!(e.d.lock().a, 20);
/// assert_eq!(e.d.lock().b, 30);
/// # Ok::<(), Error>(())
/// ```
///
/// The following example shows how to use interior mutability to modify the contents of a struct
/// protected by a spinlock despite only having a shared reference:
///
/// ```
/// use kernel::sync::SpinLock;
///
/// struct Example {
///     a: u32,
///     b: u32,
/// }
///
/// fn example(m: &SpinLock<Example>) {
///     let mut guard = m.lock();
///     guard.a += 10;
///     guard.b += 20;
/// }
/// ```
///
/// [`spinlock_t`]: ../../../../include/linux/spinlock.h
#[cfg(not(CONFIG_RROS_SPINLOCK))]
pub type SpinLock<T> = super::Lock<T, SpinLockBackend>;

/// A kernel `spinlock_t` lock backend.
pub struct SpinLockBackend;

// SAFETY: The underlying kernel `spinlock_t` object ensures mutual exclusion. `relock` uses the
// default implementation that always calls the same locking method.
unsafe impl super::Backend for SpinLockBackend {
    type State = bindings::spinlock_t;
    type GuardState = ();

    unsafe fn init(
        ptr: *mut Self::State,
        name: *const core::ffi::c_char,
        key: *mut bindings::lock_class_key,
    ) {
        // SAFETY: The safety requirements ensure that `ptr` is valid for writes, and `name` and
        // `key` are valid for read indefinitely.
        unsafe { bindings::spin_lock_init(ptr, name, key) }
    }

    unsafe fn lock(ptr: *mut Self::State) -> Self::GuardState {
        // SAFETY: The safety requirements of this function ensure that `ptr` points to valid
        // memory, and that it has been initialised before.
        unsafe { bindings::spin_lock(ptr) }
    }

    unsafe fn unlock(ptr: *mut Self::State, _guard_state: &Self::GuardState) {
        // SAFETY: The safety requirements of this function ensure that `ptr` is valid and that the
        // caller is the owner of the mutex.
        unsafe { bindings::spin_unlock(ptr) }
    }
}

#[cfg(CONFIG_RROS)]
extern "C" {
    #[allow(improper_ctypes)]
    fn rust_helper_spin_lock_init(
        lock: *mut bindings::spinlock_t,
        name: *const c_types::c_char,
        key: *mut bindings::lock_class_key,
    );
    #[allow(dead_code)]
    fn rust_helper_spin_lock(lock: *mut bindings::spinlock);
    #[allow(dead_code)]
    fn rust_helper_spin_unlock(lock: *mut bindings::spinlock);
    fn rust_helper_hard_spin_lock(lock: *mut bindings::raw_spinlock);
    fn rust_helper_hard_spin_unlock(lock: *mut bindings::raw_spinlock);
    fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
    fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
    fn rust_helper_raw_spin_lock_init(lock: *mut bindings::raw_spinlock_t);
    fn rust_helper_raw_spin_lock(lock: *mut bindings::hard_spinlock_t);
    fn rust_helper_raw_spin_unlock(lock: *mut bindings::hard_spinlock_t);
    fn rust_helper_raw_spin_lock_nested(lock: *mut bindings::hard_spinlock_t, depth: u32);
}

/// Safely initialises a [`SpinLock`] with the given name, generating a new lock class.
#[cfg(CONFIG_RROS)]
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
#[cfg(CONFIG_RROS_SPINLOCK)]
pub struct SpinLock<T: ?Sized> {
    spin_lock: Opaque<bindings::spinlock>,

    /// Spinlocks are architecture-defined. So we conservatively require them to be pinned in case
    /// some architecture uses self-references now or in the future.
    _pin: PhantomPinned,

    data: UnsafeCell<T>,
}

// SAFETY: `SpinLock` can be transferred across thread boundaries iff the data it protects can.
#[cfg(CONFIG_RROS_SPINLOCK)]
unsafe impl<T: ?Sized + Send> Send for SpinLock<T> {}

// SAFETY: `SpinLock` serialises the interior mutability it provides, so it is `Sync` as long as the
// data it protects is `Send`.
#[cfg(CONFIG_RROS_SPINLOCK)]
unsafe impl<T: ?Sized + Send> Sync for SpinLock<T> {}

#[cfg(CONFIG_RROS_SPINLOCK)]
impl<T> SpinLock<T> {
    /// Constructs a new spinlock.
    ///
    /// # Safety
    ///
    /// The caller must call [`SpinLock::init`] before using the spinlock.
    pub const unsafe fn new(t: T) -> Self {
        Self {
            spin_lock: Opaque::uninit(),
            data: UnsafeCell::new(t),
            _pin: PhantomPinned,
        }
    }
}

#[cfg(not (CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized> SpinLock<T> {
   
    /// The `irq_lock_noguard` method acquires the lock and disables interrupts, but does not return a `Guard`. Instead, it returns a `u64` that represents the previous interrupt state. This method is unsafe because it does not provide any guarantees about the lifetime of the lock.
    // FIXME: use this to enable the smp function
    pub fn irq_lock_noguard(&self) -> u64 {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {

            rust_helper_raw_spin_lock_irqsave(self.state.get()  as *mut bindings::hard_spinlock_t)
        
        }
    }

    /// The `irq_unlock_noguard` method releases the lock and restores the interrupt state to the value given by `flags`. This method is unsafe because it does not check whether the lock is currently held by the caller.
    // FIXME: use this to enable the smp function
    pub fn irq_unlock_noguard(&self, flags: u64) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                self.state.get()  as *mut bindings::hard_spinlock_t,
                flags,
            );
        }
    }

    /// The `raw_spin_lock` method acquires the lock.
    pub fn raw_spin_lock(&self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe { rust_helper_raw_spin_lock(self.state.get()  as *mut bindings::hard_spinlock_t) }
    }

        /// The `raw_spin_lock_nested` method acquires the lock nestly.
    pub fn raw_spin_lock_nested(&self, depth: u32) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_lock_nested(
                self.state.get()  as *mut bindings::hard_spinlock_t,
                depth,
            )
        }
    }

    /// The `raw_spin_unlock` method release the lock.
    pub fn raw_spin_unlock(&self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock(self.state.get()  as *mut bindings::hard_spinlock_t)
        }
    }
}

#[cfg(CONFIG_RROS_SPINLOCK)]
impl<T: ?Sized> NeedsLockClass for SpinLock<T> {
    unsafe fn init(self: Pin<&mut Self>, name: &'static CStr, key: *mut bindings::lock_class_key) {
        // SAFETY: The caller guarantees that `name` and `key` are initialised. So the pointers are valid.
        unsafe { rust_helper_spin_lock_init(self.spin_lock.get(), name.as_char_ptr(), key) };
    }
}

#[cfg(not (CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized> Lock for SpinLock<T> {
    type Inner = T;

    fn lock_noguard(&self) {
        // SAFETY: `spin_lock` points to valid memory.
        // unsafe { rust_helper_spin_lock(self.spin_lock.get()) };
        unsafe { rust_helper_hard_spin_lock(self.state.get() as *mut bindings::raw_spinlock) };
        // unsafe { rust_helper_hard_spin_lock((*self.spin_lock.get()).rlock()
        // as *mut bindings::raw_spinlock) };
    }

    unsafe fn unlock(&self) {
        // SAFETY: `spin_lock` points to valid memory.
        // unsafe { rust_helper_spin_unlock(self.spin_lock.get()) };
        unsafe {
            rust_helper_hard_spin_unlock(self.state.get() as *mut bindings::raw_spinlock)
        };
        // unsafe { rust_helper_hard_spin_unlock((*self.spin_lock.get()).rlock()
        // as *mut bindings::raw_spinlock) };
    }

    fn locked_data(&self) -> &UnsafeCell<T> {
        // SAFETY: The caller guarantees that self is initialised.
        &self.data
    }
}

/// A wrapper for [`hard_spinlock_t`].
#[cfg(CONFIG_RROS)]
#[repr(transparent)]
pub struct HardSpinlock {
    lock: bindings::hard_spinlock_t,
}

#[cfg(CONFIG_RROS)]
impl HardSpinlock {
    /// Constructs a new struct.
    pub fn new() -> Self {
        HardSpinlock {
            lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map::default(),
            },
        }
    }

    /// Initialize Self.
    pub fn init(&mut self) {
        self.lock = bindings::hard_spinlock_t::default();
        // SAFETY: `self.lock` points to valid memory.
        unsafe {
            rust_helper_raw_spin_lock_init(
                &mut self.lock as *mut bindings::hard_spinlock_t as *mut bindings::raw_spinlock_t,
            );
        }
    }

    /// Call `Linux` `raw_spin_lock_irqsave` to lock.
    pub fn raw_spin_lock_irqsave(&mut self) -> u64 {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_lock_irqsave(&mut self.lock as *mut bindings::hard_spinlock_t)
        }
    }

    /// Call `Linux` `raw_spin_unlock_irqrestore` to unlock.
    pub fn raw_spin_unlock_irqrestore(&mut self, flags: u64) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                &mut self.lock as *mut bindings::hard_spinlock_t,
                flags,
            );
        }
    }

    /// Call `Linux` `raw_spin_lock` to lock.
    pub fn raw_spin_lock(&mut self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_lock(&mut self.lock as *mut bindings::hard_spinlock_t);
        }
    }

    /// Call `Linux` `raw_spin_unlock` to unlock.
    pub fn raw_spin_unlock(&mut self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock(&mut self.lock as *mut bindings::hard_spinlock_t);
        }
    }

    /// Call `Linux` `raw_spin_lock_nested` to lock nestly.
    pub fn raw_spin_lock_nested(&mut self, depth: u32) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_lock_nested(
                &mut self.lock as *mut bindings::hard_spinlock_t,
                depth,
            )
        }
    }
}

/// A wrapper for [`raw_spinlock_t`].
#[cfg(CONFIG_RROS)]
#[repr(transparent)]
pub struct RawSpinLock {
    #[allow(dead_code)]
    lock: bindings::raw_spinlock_t,
}
