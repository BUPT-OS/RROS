// SPDX-License-Identifier: GPL-2.0

//! Tasks (threads and processes).
//!
//! C header: [`include/linux/sched.h`](../../../../include/linux/sched.h).

use crate::{bindings, c_types, types::ARef, types::Opaque};
use core::{marker::PhantomData, ops::Deref, ptr};

extern "C" {
    #[allow(improper_ctypes)]
    fn rust_helper_signal_pending(t: *const bindings::task_struct) -> c_types::c_int;
    #[allow(improper_ctypes)]
    fn rust_helper_get_current() -> *mut bindings::task_struct;
    #[allow(improper_ctypes)]
    fn rust_helper_get_task_struct(t: *mut bindings::task_struct);
    #[allow(improper_ctypes)]
    fn rust_helper_put_task_struct(t: *mut bindings::task_struct);
    fn rust_helper_next_task(curr: *mut bindings::task_struct) -> *mut bindings::task_struct;
}

use core::iter::Iterator;

/// An possible CPU index iterator.
///
/// This iterator has a similar abilitiy to the kernel's macro `for_each_possible_cpu`.
pub struct EachProcessIter {
    init: *mut bindings::task_struct,
    iter: ARef<Task>
}

impl Iterator for EachProcessIter {
    type Item = ARef<Task>;

    fn next(&mut self) -> Option<ARef<Task>> {
        let next_task =
            unsafe { rust_helper_next_task((*self.iter).0.get()) };
        // When [`bindings::cpumask_next`] can not find further CPUs set in the
        // [`bindings::__cpu_possible_mask`], it returns a value >= [`bindings::nr_cpu_ids`].
        if next_task == self.init {
            return None;
        } 
        // self.iter = (&*TaskRef::from_ptr(next_task)).into();
        self.iter = unsafe{(&*(next_task as *const _ as *const Task)).into()};
        Some(self.iter.clone())
    }
}

pub fn all_threads() -> EachProcessIter {
    // Initial index is set to -1. Since [`bindings::cpumask_next`] return the next set bit in a
    // [`bindings::__cpu_possible_mask`], the CPU index should begins from 0.
    EachProcessIter {
        init: unsafe{&mut bindings::init_task},
        iter: unsafe{(&*(&bindings::init_task as *const _ as *const Task)).into()}
    }
}

/// Wraps the kernel's `struct task_struct`.
///
/// # Invariants
///
/// All instances are valid tasks created by the C portion of the kernel.
///
/// Instances of this type are always refcounted, that is, a call to `get_task_struct` ensures
/// that the allocation remains valid at least until the matching call to `put_task_struct`.
///
/// # Examples
///
/// The following is an example of getting the PID of the current thread with zero additional cost
/// when compared to the C version:
///
/// ```
/// let pid = current!().pid();
/// ```
///
/// Getting the PID of the current process, also zero additional cost:
///
/// ```
/// let pid = current!().group_leader().pid();
/// ```
///
/// Getting the current task and storing it in some struct. The reference count is automatically
/// incremented when creating `State` and decremented when it is dropped:
///
/// ```
/// use kernel::{task::Task, types::ARef};
///
/// struct State {
///     creator: ARef<Task>,
///     index: u32,
/// }
///
/// impl State {
///     fn new() -> Self {
///         Self {
///             creator: current!().into(),
///             index: 0,
///         }
///     }
/// }
/// ```
#[repr(transparent)]
pub struct Task(pub(crate) Opaque<bindings::task_struct>);

// SAFETY: Given that the task is referenced, it is OK to send it to another thread.
unsafe impl Send for Task {}

// SAFETY: It's OK to access `Task` through references from other threads because we're either
// accessing properties that don't change (e.g., `pid`, `group_leader`) or that are properly
// synchronised by C code (e.g., `signal_pending`).
unsafe impl Sync for Task {}

/// The type of process identifiers (PIDs).
type Pid = bindings::pid_t;

impl Task {
/// Returns a task reference for the currently executing task/thread.
    ///
    /// The recommended way to get the current task/thread is to use the
    /// [`current`] macro because it is safe.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the returned object doesn't outlive the current task/thread.
    // TODO: add unsafe, tmp removed for testing
    pub fn current() -> impl Deref<Target = Task> {
        struct TaskRef<'a> {
            task: &'a Task,
            _not_send: PhantomData<*mut ()>,
        }

        impl Deref for TaskRef<'_> {
            type Target = Task;

            fn deref(&self) -> &Self::Target {
                self.task
            }
        }

        // SAFETY: Just an FFI call with no additional safety requirements.
        let ptr = unsafe { rust_helper_get_current() };

        TaskRef {
            // SAFETY: If the current thread is still running, the current task is valid. Given
            // that `TaskRef` is not `Send`, we know it cannot be transferred to another thread
            // (where it could potentially outlive the caller).
            task: unsafe { &*ptr.cast() },
            _not_send: PhantomData,
        }
    }

    /// The `current_ptr` function returns a raw pointer to the current task. It is unsafe and should be used with caution.
    pub fn current_ptr() -> *mut bindings::task_struct {
        unsafe { rust_helper_get_current() }
    }

    pub fn group_leader(&self) -> &Task {
        // SAFETY: By the type invariant, we know that `self.0` is a valid task. Valid tasks always
        // have a valid `group_leader`.
        let ptr = unsafe { *ptr::addr_of!((*self.0.get()).group_leader) };

        // SAFETY: The lifetime of the returned task reference is tied to the lifetime of `self`,
        // and given that a task has a reference to its group leader, we know it must be valid for
        // the lifetime of the returned task reference.
        unsafe { &*ptr.cast() }
    }

    /// Returns the PID of the given task.
    pub fn pid(&self) -> Pid {
        // SAFETY: By the type invariant, we know that `self.0.get()` is non-null and valid.
        unsafe { (*self.0.get()).pid }
    }

    /// The `kernel` function checks if the given task is a kernel task. It returns true if the memory descriptor of the task is null, indicating that it's a kernel task.
    pub fn kernel(&self) -> bool {
        unsafe { (*self.0.get()).mm == core::ptr::null_mut() }
    }

    /// The `state` function returns the state of the given task as a u32. It is unsafe because it directly accesses the `state` field of the task struct.
    pub fn state(&self) -> u32 {
        unsafe { (*self.0.get()).state as u32 }
    }

    /// Returns the CPU of the given task as a u32.
    /// This function is unsafe because it directly accesses the `cpu` field of the task struct.
    pub fn cpu(&self) -> u32 {
        unsafe { (*self.0.get()).cpu as u32 }
    }

    /// Determines whether the given task has pending signals.
    pub fn signal_pending(&self) -> bool {
        // SAFETY: By the type invariant, we know that `self.0.get()` is non-null and valid.
        unsafe { rust_helper_signal_pending(self.0.get()) != 0 }
    }

    /// Call `Linux` wake_up_process.
    pub fn wake_up_process(ptr: *mut bindings::task_struct) -> i32 {
        unsafe { bindings::wake_up_process(ptr) }
    }
}

impl PartialEq for Task {
    fn eq(&self, other: &Self) -> bool {
        self.0.get() == other.0.get()
    }
}

impl Eq for Task {}

// impl Clone for Task {
//     fn clone(&self) -> Self {
//         // SAFETY: The type invariants guarantee that `self.0.get()` has a non-zero reference count.
//         unsafe { rust_helper_get_task_struct(self.0.get()) };

//         // INVARIANT: We incremented the reference count to account for the new `Task` being
//         // created.
//         Self { ptr: self.0.get() }
//     }
// }

// impl Drop for Task {
//     fn drop(&mut self) {
//         // INVARIANT: We may decrement the refcount to zero, but the `Task` is being dropped, so
//         // this is not observable.
//         // SAFETY: The type invariants guarantee that `Task::ptr` has a non-zero reference count.
//         unsafe { rust_helper_put_task_struct(self.0.get()) };
//     }
// }

unsafe impl crate::types::AlwaysRefCounted for Task {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { rust_helper_get_task_struct(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { rust_helper_put_task_struct(obj.cast().as_ptr()) }
    }
}

// /// A wrapper for [`Task`] that doesn't automatically decrement the refcount when dropped.
// ///
// /// We need the wrapper because [`ManuallyDrop`] alone would allow callers to call
// /// [`ManuallyDrop::into_inner`]. This would allow an unsafe sequence to be triggered without
// /// `unsafe` blocks because it would trigger an unbalanced call to `put_task_struct`.
// ///
// /// We make this explicitly not [`Send`] so that we can use it to represent the current thread
// /// without having to increment/decrement its reference count.
// ///
// /// # Invariants
// ///
// /// The wrapped [`Task`] remains valid for the lifetime of the object.
// pub struct TaskRef<'a> {
//     task: ManuallyDrop<Task>,
//     _not_send: PhantomData<(&'a (), *mut ())>,
// }

// impl TaskRef<'_> {
//     /// Constructs a new `struct task_struct` wrapper that doesn't change its reference count.
//     ///
//     /// # Safety
//     ///
//     /// The pointer `ptr` must be non-null and valid for the lifetime of the object.
//     pub(crate) unsafe fn from_ptr(ptr: *mut bindings::task_struct) -> Self {
//         Self {
//             task: ManuallyDrop::new(Task { ptr }),
//             _not_send: PhantomData,
//         }
//     }
// }

// // SAFETY: It is OK to share a reference to the current thread with another thread because we know
// // the owner cannot go away while the shared reference exists (and `Task` itself is `Sync`).
// unsafe impl Sync for TaskRef<'_> {}

// impl Deref for TaskRef<'_> {
//     type Target = Task;

//     fn deref(&self) -> &Self::Target {
//         self.task.deref()
//     }
// }
