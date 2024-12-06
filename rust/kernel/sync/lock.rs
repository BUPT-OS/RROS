// SPDX-License-Identifier: GPL-2.0

//! Generic kernel lock and guard.
//!
//! It contains a generic Rust lock and guard that allow for different backends (e.g., mutexes,
//! spinlocks, raw spinlocks) to be provided with minimal effort.

use super::LockClassKey;
use crate::{bindings, init::PinInit, pin_init, str::CStr, types::Opaque, types::ScopeGuard};
use core::{cell::UnsafeCell, marker::PhantomData, marker::PhantomPinned, pin::Pin};
use macros::pin_data;

pub mod mutex;
pub mod spinlock;

#[cfg(CONFIG_RROS)]
use crate::c_types;

/// The "backend" of a lock.
///
/// It is the actual implementation of the lock, without the need to repeat patterns used in all
/// locks.
///
/// # Safety
///
/// - Implementers must ensure that only one thread/CPU may access the protected data once the lock
/// is owned, that is, between calls to `lock` and `unlock`.
/// - Implementers must also ensure that `relock` uses the same locking method as the original
/// lock operation.
pub unsafe trait Backend {
    /// The state required by the lock.
    type State;

    /// The state required to be kept between lock and unlock.
    type GuardState;

    /// Initialises the lock.
    ///
    /// # Safety
    ///
    /// `ptr` must be valid for write for the duration of the call, while `name` and `key` must
    /// remain valid for read indefinitely.
    unsafe fn init(
        ptr: *mut Self::State,
        name: *const core::ffi::c_char,
        key: *mut bindings::lock_class_key,
    );

    /// Acquires the lock, making the caller its owner.
    ///
    /// # Safety
    ///
    /// Callers must ensure that [`Backend::init`] has been previously called.
    #[must_use]
    unsafe fn lock(ptr: *mut Self::State) -> Self::GuardState;

    /// Releases the lock, giving up its ownership.
    ///
    /// # Safety
    ///
    /// It must only be called by the current owner of the lock.
    unsafe fn unlock(ptr: *mut Self::State, guard_state: &Self::GuardState);

    /// Reacquires the lock, making the caller its owner.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `guard_state` comes from a previous call to [`Backend::lock`] (or
    /// variant) that has been unlocked with [`Backend::unlock`] and will be relocked now.
    unsafe fn relock(ptr: *mut Self::State, guard_state: &mut Self::GuardState) {
        // SAFETY: The safety requirements ensure that the lock is initialised.
        *guard_state = unsafe { Self::lock(ptr) };
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

/// A mutual exclusion primitive.
///
/// Exposes one of the kernel locking primitives. Which one is exposed depends on the lock
/// [`Backend`] specified as the generic parameter `B`.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
#[pin_data]
pub struct Lock<T: ?Sized, B: Backend> {
    /// The kernel lock object.
    #[pin]
    state: Opaque<B::State>,

    /// Some locks are known to be self-referential (e.g., mutexes), while others are architecture
    /// or config defined (e.g., spinlocks). So we conservatively require them to be pinned in case
    /// some architecture uses self-references now or in the future.
    #[pin]
    _pin: PhantomPinned,

    /// The data protected by the lock.
    pub(crate) data: UnsafeCell<T>,
}

// SAFETY: `Lock` can be transferred across thread boundaries iff the data it protects can.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
unsafe impl<T: ?Sized + Send, B: Backend> Send for Lock<T, B> {}

// SAFETY: `Lock` serialises the interior mutability it provides, so it is `Sync` as long as the
// data it protects is `Send`.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
unsafe impl<T: ?Sized + Send, B: Backend> Sync for Lock<T, B> {}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T, B: Backend> Lock<T, B> {
    /// Constructs a new lock initialiser.
    #[allow(clippy::new_ret_no_self)]
    pub fn new(t: T, name: &'static CStr, key: &'static LockClassKey) -> impl PinInit<Self> {
        pin_init!(Self {
            data: UnsafeCell::new(t),
            _pin: PhantomPinned,
            // SAFETY: `slot` is valid while the closure is called and both `name` and `key` have
            // static lifetimes so they live indefinitely.
            state <- Opaque::ffi_init(|slot| unsafe {
                B::init(slot, name.as_char_ptr(), key.as_ptr())
            }),
        })
    }
}



#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized, B: Backend> Lock<T, B> {
    /// Acquires the lock and gives the caller access to the data protected by it.
    pub fn lock(&self) -> Guard<'_, T, B> {
        // SAFETY: The constructor of the type calls `init`, so the existence of the object proves
        // that `init` was called.
        let state = unsafe { B::lock(self.state.get()) };
        // SAFETY: The lock was just acquired.
        unsafe { Guard::new(self, state) }
    }

    pub fn lock_noguard(&self) {
        // SAFETY: `spin_lock` points to valid memory.
        // unsafe { rust_helper_spin_lock(self.spin_lock.get()) };
        unsafe { B::lock(self.state.get()) };
        // unsafe { rust_helper_hard_spin_lock((*self.spin_lock.get()).rlock()
        // as *mut bindings::raw_spinlock) };
    }

    pub fn unlock(&self, guard_state: &B::GuardState) {
        // SAFETY: `spin_lock` points to valid memory.
        // unsafe { rust_helper_spin_unlock(self.spin_lock.get()) };
        //TODO: 
        unsafe {
            B::unlock(self.state.get(),guard_state);
        };
        // unsafe { rust_helper_hard_spin_unlock((*self.spin_lock.get()).rlock()
        // as *mut bindings::raw_spinlock) };
    }

    pub fn locked_data(&self) -> &UnsafeCell<T> {
        // SAFETY: The caller guarantees that self is initialised.
        &self.data
    }

    /// The `irq_lock_noguard` method acquires the lock and disables interrupts, but does not return a `Guard`. Instead, it returns a `u64` that represents the previous interrupt state. This method is unsafe because it does not provide any guarantees about the lifetime of the lock.
    // FIXME: use this to enable the smp function
    pub fn irq_lock_noguard(&self) -> u64 {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {

            rust_helper_raw_spin_lock_irqsave(self.lock().lock as *const Lock<_, _> as *mut bindings::hard_spinlock_t)
        
        }
    }

    /// The `irq_unlock_noguard` method releases the lock and restores the interrupt state to the value given by `flags`. This method is unsafe because it does not check whether the lock is currently held by the caller.
    // FIXME: use this to enable the smp function
    pub fn irq_unlock_noguard(&self, flags: u64) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                self.lock().lock as *const Lock<_, _> as *mut bindings::hard_spinlock_t,
                flags,
            );
        }
    }

    /// The `raw_spin_lock` method acquires the lock.
    pub fn raw_spin_lock(&self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe { rust_helper_raw_spin_lock(self.lock().lock as *const Lock<_, _> as *mut bindings::hard_spinlock_t) }
    }

        /// The `raw_spin_lock_nested` method acquires the lock nestly.
    pub fn raw_spin_lock_nested(&self, depth: u32) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_lock_nested(
                self.lock().lock as *const Lock<_, _> as *mut bindings::hard_spinlock_t,
                depth,
            )
        }
    }

    /// The `raw_spin_unlock` method release the lock.
    pub fn raw_spin_unlock(&self) {
        // SAFETY: The caller guarantees that self is initialised. So the pointer is valid.
        unsafe {
            rust_helper_raw_spin_unlock(self.lock().lock as *const Lock<_, _> as *mut bindings::hard_spinlock_t)
        }
    }

}

/// A lock guard.
///
/// Allows mutual exclusion primitives that implement the [`Backend`] trait to automatically unlock
/// when a guard goes out of scope. It also provides a safe and convenient way to access the data
/// protected by the lock.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
#[must_use = "the lock unlocks immediately when the guard is unused"]
pub struct Guard<'a, T: ?Sized, B: Backend> {
    pub(crate) lock: &'a Lock<T, B>,
    pub(crate) state: B::GuardState,
    _not_send: PhantomData<*mut ()>,
}

// SAFETY: `Guard` is sync when the data protected by the lock is also sync.
#[cfg(not(CONFIG_RROS_SPINLOCK))]
unsafe impl<T: Sync + ?Sized, B: Backend> Sync for Guard<'_, T, B> {}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized, B: Backend> Guard<'_, T, B> {
    pub(crate) fn do_unlocked(&mut self, cb: impl FnOnce()) {
        // SAFETY: The caller owns the lock, so it is safe to unlock it.
        unsafe { B::unlock(self.lock.state.get(), &self.state) };

        // SAFETY: The lock was just unlocked above and is being relocked now.
        let _relock =
            ScopeGuard::new(|| unsafe { B::relock(self.lock.state.get(), &mut self.state) });

        cb();
    }
}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized, B: Backend> core::ops::Deref for Guard<'_, T, B> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: The caller owns the lock, so it is safe to deref the protected data.
        unsafe { &*self.lock.data.get() }
    }
}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized, B: Backend> core::ops::DerefMut for Guard<'_, T, B> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: The caller owns the lock, so it is safe to deref the protected data.
        unsafe { &mut *self.lock.data.get() }
    }
}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<T: ?Sized, B: Backend> Drop for Guard<'_, T, B> {
    fn drop(&mut self) {
        // SAFETY: The caller owns the lock, so it is safe to unlock it.
        unsafe { B::unlock(self.lock.state.get(), &self.state) };
    }
}

#[cfg(not(CONFIG_RROS_SPINLOCK))]
impl<'a, T: ?Sized, B: Backend> Guard<'a, T, B> {
    /// Constructs a new immutable lock guard.
    ///
    /// # Safety
    ///
    /// The caller must ensure that it owns the lock.
    pub(crate) unsafe fn new(lock: &'a Lock<T, B>, state: B::GuardState) -> Self {
        Self {
            lock,
            state,
            _not_send: PhantomData,
        }
    }
}

extern "C" {
    fn rust_helper_cond_resched() -> c_types::c_int;
}

/// Safely initialises an object that has an `init` function that takes a name and a lock class as
/// arguments, examples of these are [`Mutex`] and [`SpinLock`]. Each of them also provides a more
/// specialised name that uses this macro.
#[cfg(CONFIG_RROS)]
#[doc(hidden)]
#[macro_export]
macro_rules! init_with_lockdep {
    ($obj:expr, $name:expr) => {{
        static mut CLASS: core::mem::MaybeUninit<$crate::bindings::lock_class_key> =
            core::mem::MaybeUninit::uninit();
        let obj = $obj;
        let name = $crate::c_str!($name);
        // SAFETY: `CLASS` is never used by Rust code directly; the kernel may change it though.
        #[allow(unused_unsafe)]
        unsafe {
            $crate::sync::NeedsLockClass::init(obj, name, CLASS.as_mut_ptr())
        };
    }};
}

/// A trait for types that need a lock class during initialisation.
///
/// Implementers of this trait benefit from the [`init_with_lockdep`] macro that generates a new
/// class for each initialisation call site.
#[cfg(CONFIG_RROS)]
pub trait NeedsLockClass {
    /// Initialises the type instance so that it can be safely used.
    ///
    /// Callers are encouraged to use the [`init_with_lockdep`] macro as it automatically creates a
    /// new lock class on each usage.
    ///
    /// # Safety
    ///
    /// `key` must point to a valid memory location as it will be used by the kernel.
    unsafe fn init(self: Pin<&mut Self>, name: &'static CStr, key: *mut bindings::lock_class_key);
}

/// Reschedules the caller's task if needed.
#[cfg(CONFIG_RROS_SPINLOCK)]
pub fn cond_resched() -> bool {
    // SAFETY: No arguments, reschedules `current` if needed.
    unsafe { rust_helper_cond_resched() != 0 }
}

/// Automatically initialises static instances of synchronisation primitives.
///
/// The syntax resembles that of regular static variables, except that the value assigned is that
/// of the protected type (if one exists). In the examples below, all primitives except for
/// [`CondVar`] require the inner value to be supplied.
///
/// # Examples
///
/// ```ignore
/// # use kernel::{init_static_sync, sync::{CondVar, Mutex, RevocableMutex, SpinLock}};
/// struct Test {
///     a: u32,
///     b: u32,
/// }
///
/// init_static_sync! {
///     static A: Mutex<Test> = Test { a: 10, b: 20 };
///
///     /// Documentation for `B`.
///     pub static B: Mutex<u32> = 0;
///
///     pub(crate) static C: SpinLock<Test> = Test { a: 10, b: 20 };
///     static D: CondVar;
///
///     static E: RevocableMutex<Test> = Test { a: 30, b: 40 };
/// }
/// ```
#[cfg(CONFIG_RROS)]
#[macro_export]
macro_rules! init_static_sync {
    ($($(#[$outer:meta])* $v:vis static $id:ident : $t:ty $(= $value:expr)?;)*) => {
        $(
            $(#[$outer])*
            $v static $id: $t = {
                #[link_section = ".ctors"]
                #[used]
                static TMP: extern "C" fn() = {
                    extern "C" fn constructor() {
                        // SAFETY: This locally-defined function is only called from a constructor,
                        // which guarantees that `$id` is not accessible from other threads
                        // concurrently.
                        #[allow(clippy::cast_ref_to_mut)]
                        let mutable = unsafe { &mut *(&$id as *const _ as *mut $t) };
                        // SAFETY: It's a shared static, so it cannot move.
                        let pinned = unsafe { core::pin::Pin::new_unchecked(mutable) };
                        $crate::init_with_lockdep!(pinned, stringify!($id));
                    }
                    constructor
                };
                $crate::init_static_sync!(@call_new $t, $($value)?)
            };
        )*
    };
    (@call_new $t:ty, $value:expr) => {{
        let v = $value;
        // SAFETY: the initialisation function is called by the constructor above.
        unsafe { <$t>::new(v) }
    }};
    (@call_new $t:ty,) => {
        // SAFETY: the initialisation function is called by the constructor above.
        unsafe { <$t>::new() }
    };
}
