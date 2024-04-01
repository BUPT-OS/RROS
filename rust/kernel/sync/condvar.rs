// SPDX-License-Identifier: GPL-2.0

//! A condition variable.
//!
//! This module allows Rust code to use the kernel's [`struct wait_queue_head`] as a condition
//! variable.

use super::{lock::Backend, lock::Guard, LockClassKey};
use crate::{
    bindings,
    init::PinInit,
    pin_init,
    str::CStr,
    task::{MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE, TASK_NORMAL, TASK_UNINTERRUPTIBLE},
    time::Jiffies,
    types::Opaque,
};
use core::ffi::{c_int, c_long};
use core::marker::PhantomPinned;
use core::ptr;
use macros::pin_data;

/// Creates a [`CondVar`] initialiser with the given name and a newly-created lock class.
#[macro_export]
macro_rules! new_condvar {
    ($($name:literal)?) => {
        $crate::sync::CondVar::new($crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}
pub use new_condvar;

/// A conditional variable.
///
/// Exposes the kernel's [`struct wait_queue_head`] as a condition variable. It allows the caller to
/// atomically release the given lock and go to sleep. It reacquires the lock when it wakes up. And
/// it wakes up when notified by another thread (via [`CondVar::notify_one`] or
/// [`CondVar::notify_all`]) or because the thread received a signal. It may also wake up
/// spuriously.
///
/// Instances of [`CondVar`] need a lock class and to be pinned. The recommended way to create such
/// instances is with the [`pin_init`](crate::pin_init) and [`new_condvar`] macros.
///
/// # Examples
///
/// The following is an example of using a condvar with a mutex:
///
/// ```
/// use kernel::sync::{new_condvar, new_mutex, CondVar, Mutex};
///
/// #[pin_data]
/// pub struct Example {
///     #[pin]
///     value: Mutex<u32>,
///
///     #[pin]
///     value_changed: CondVar,
/// }
///
/// /// Waits for `e.value` to become `v`.
/// fn wait_for_value(e: &Example, v: u32) {
///     let mut guard = e.value.lock();
///     while *guard != v {
///         e.value_changed.wait(&mut guard);
///     }
/// }
///
/// /// Increments `e.value` and notifies all potential waiters.
/// fn increment(e: &Example) {
///     *e.value.lock() += 1;
///     e.value_changed.notify_all();
/// }
///
/// /// Allocates a new boxed `Example`.
/// fn new_example() -> Result<Pin<Box<Example>>> {
///     Box::pin_init(pin_init!(Example {
///         value <- new_mutex!(0),
///         value_changed <- new_condvar!(),
///     }))
/// }
/// ```
///
/// [`struct wait_queue_head`]: srctree/include/linux/wait.h
#[pin_data]
pub struct CondVar {
    #[pin]
    pub(crate) wait_queue_head: Opaque<bindings::wait_queue_head>,

    /// A condvar needs to be pinned because it contains a [`struct list_head`] that is
    /// self-referential, so it cannot be safely moved once it is initialised.
    ///
    /// [`struct list_head`]: srctree/include/linux/types.h
    #[pin]
    _pin: PhantomPinned,
}

// SAFETY: `CondVar` only uses a `struct wait_queue_head`, which is safe to use on any thread.
#[allow(clippy::non_send_fields_in_send_ty)]
unsafe impl Send for CondVar {}

// SAFETY: `CondVar` only uses a `struct wait_queue_head`, which is safe to use on multiple threads
// concurrently.
unsafe impl Sync for CondVar {}

impl CondVar {
    /// Constructs a new condvar initialiser.
    pub fn new(name: &'static CStr, key: &'static LockClassKey) -> impl PinInit<Self> {
        pin_init!(Self {
            _pin: PhantomPinned,
            // SAFETY: `slot` is valid while the closure is called and both `name` and `key` have
            // static lifetimes so they live indefinitely.
            wait_queue_head <- Opaque::ffi_init(|slot| unsafe {
                bindings::__init_waitqueue_head(slot, name.as_char_ptr(), key.as_ptr())
            }),
        })
    }

    fn wait_internal<T: ?Sized, B: Backend>(
        &self,
        wait_state: c_int,
        guard: &mut Guard<'_, T, B>,
        timeout_in_jiffies: c_long,
    ) -> c_long {
        let wait = Opaque::<bindings::wait_queue_entry>::uninit();

        // SAFETY: `wait` points to valid memory.
        unsafe { bindings::init_wait(wait.get()) };

        // SAFETY: Both `wait` and `wait_queue_head` point to valid memory.
        unsafe {
            bindings::prepare_to_wait_exclusive(self.wait_queue_head.get(), wait.get(), wait_state)
        };

        // SAFETY: Switches to another thread. The timeout can be any number.
        let ret = guard.do_unlocked(|| unsafe { bindings::schedule_timeout(timeout_in_jiffies) });

        // SAFETY: Both `wait` and `wait_queue_head` point to valid memory.
        unsafe { bindings::finish_wait(self.wait_queue_head.get(), wait.get()) };

        ret
    }

    /// Releases the lock and waits for a notification in uninterruptible mode.
    ///
    /// Atomically releases the given lock (whose ownership is proven by the guard) and puts the
    /// thread to sleep, reacquiring the lock on wake up. It wakes up when notified by
    /// [`CondVar::notify_one`] or [`CondVar::notify_all`]. Note that it may also wake up
    /// spuriously.
    pub fn wait<T: ?Sized, B: Backend>(&self, guard: &mut Guard<'_, T, B>) {
        self.wait_internal(TASK_UNINTERRUPTIBLE, guard, MAX_SCHEDULE_TIMEOUT);
    }

    /// Releases the lock and waits for a notification in interruptible mode.
    ///
    /// Similar to [`CondVar::wait`], except that the wait is interruptible. That is, the thread may
    /// wake up due to signals. It may also wake up spuriously.
    ///
    /// Returns whether there is a signal pending.
    #[must_use = "wait_interruptible returns if a signal is pending, so the caller must check the return value"]
    pub fn wait_interruptible<T: ?Sized, B: Backend>(&self, guard: &mut Guard<'_, T, B>) -> bool {
        self.wait_internal(TASK_INTERRUPTIBLE, guard, MAX_SCHEDULE_TIMEOUT);
        crate::current!().signal_pending()
    }

    /// Releases the lock and waits for a notification in interruptible mode.
    ///
    /// Atomically releases the given lock (whose ownership is proven by the guard) and puts the
    /// thread to sleep. It wakes up when notified by [`CondVar::notify_one`] or
    /// [`CondVar::notify_all`], or when a timeout occurs, or when the thread receives a signal.
    #[must_use = "wait_interruptible_timeout returns if a signal is pending, so the caller must check the return value"]
    pub fn wait_interruptible_timeout<T: ?Sized, B: Backend>(
        &self,
        guard: &mut Guard<'_, T, B>,
        jiffies: Jiffies,
    ) -> CondVarTimeoutResult {
        let jiffies = jiffies.try_into().unwrap_or(MAX_SCHEDULE_TIMEOUT);
        let res = self.wait_internal(TASK_INTERRUPTIBLE, guard, jiffies);

        match (res as Jiffies, crate::current!().signal_pending()) {
            (jiffies, true) => CondVarTimeoutResult::Signal { jiffies },
            (0, false) => CondVarTimeoutResult::Timeout,
            (jiffies, false) => CondVarTimeoutResult::Woken { jiffies },
        }
    }

    /// Calls the kernel function to notify the appropriate number of threads.
    fn notify(&self, count: c_int) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe {
            bindings::__wake_up(
                self.wait_queue_head.get(),
                TASK_NORMAL,
                count,
                ptr::null_mut(),
            )
        };
    }

    /// Calls the kernel function to notify one thread synchronously.
    ///
    /// This method behaves like `notify_one`, except that it hints to the scheduler that the
    /// current thread is about to go to sleep, so it should schedule the target thread on the same
    /// CPU.
    pub fn notify_sync(&self) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe { bindings::__wake_up_sync(self.wait_queue_head.get(), TASK_NORMAL) };
    }

    /// Wakes a single waiter up, if any.
    ///
    /// This is not 'sticky' in the sense that if no thread is waiting, the notification is lost
    /// completely (as opposed to automatically waking up the next waiter).
    pub fn notify_one(&self) {
        self.notify(1);
    }

    /// Wakes all waiters up, if any.
    ///
    /// This is not 'sticky' in the sense that if no thread is waiting, the notification is lost
    /// completely (as opposed to automatically waking up the next waiter).
    pub fn notify_all(&self) {
        self.notify(0);
    }
}

/// The return type of `wait_timeout`.
pub enum CondVarTimeoutResult {
    /// The timeout was reached.
    Timeout,
    /// Somebody woke us up.
    Woken {
        /// Remaining sleep duration.
        jiffies: Jiffies,
    },
    /// A signal occurred.
    Signal {
        /// Remaining sleep duration.
        jiffies: Jiffies,
    },
}
