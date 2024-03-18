// SPDX-License-Identifier: GPL-2.0
// TODO: more flexible to use `trace!` macro.

//! # FTRACE USAGE IN RROS
//!
//! *[See also the ftrace usage](https://www.kernel.org/doc/html/latest/trace/ftrace.html).*
//!
//! ## Kernel Configuration
//!
//! 1. **Tracing Support**:
//!   - Enable `CONFIG_TRACING`. This is the core option for enabling the tracing infrastructure in the Linux kernel, which `ftrace` relies upon.
//!
//! 2. **Function Tracer (FTRACE)**:
//!    - Enable `CONFIG_FUNCTION_TRACER`. This option is the fundamental requirement for `ftrace` to work, as it enables tracing of function calls within the kernel.
//!
//! 3. **Tracepoints**:
//!    - Enable `CONFIG_TRACEPOINTS`. This option enables tracepoints that are statically defined in the kernel, allowing `ftrace` and other tools to hook into these points for detailed event information.
//!
//! 4. **Dynamic Ftrace**:
//!    - Enable `CONFIG_DYNAMIC_FTRACE`. This allows for the dynamic modification of kernel code to insert/remove tracepoints, making `ftrace` more versatile and with minimal overhead.
//!
//! 5. **Function Graph Tracer**:
//!    - Enable `CONFIG_FUNCTION_GRAPH_TRACER`. This option allows for graph tracing of function calls, which is useful for visualizing call stacks and understanding the flow of execution.
//!
//! 6. **Additional Tracers**:
//!    - Depending on your needs, you may also enable additional tracers, such as:
//!      - `CONFIG_SCHED_TRACER` for scheduling events,
//!      - `CONFIG_IRQSOFF_TRACER` for tracing periods where interrupts are disabled,
//!      - `CONFIG_PREEMPT_TRACER` for tracing preempt disable and enable events,
//!      - and others depending on your specific tracing requirements.
//!
//! ## Basic usage in kernel code
//!
//! Use FFI helper functions for tracepoints. The `trace_rros_*` functions are used to trace the rros events.
//!
//! ```
//! let rq = this_rros_rq();
//! ```
//!
//! Here we get the `rq` of the current CPU, and then we can use the `rq` to trace the events.
//! Then we could add the following code to trace the schedule event.
//!
//! ```
//! trace_rros_schedule(&rq);
//! ```
//!
//! ## Basic usage in user space tools
//!
//! 1. enable all the rros tracepoints in kernel.
//!
//! ```
//! echo 1 > /sys/kernel/debug/tracing/events/rros/enable
//! ```
//!
//! or enable the only one rros tracepoint.
//!
//! ```
//! echo 1 > /sys/kernel/debug/tracing/events/rros/rros_schedule/enable
//! ```
//!
//! or you could enable any tracepoints group you want.
//!
//! 2. start the program to emit the tracepoints.
//!
//! ```
//! ./{test_program}
//! ```
//!
//! 3. cat the trace.
//!
//! ```
//! cat /sys/kernel/debug/tracing/trace | grep rros
//!
//! ```
//!
//! 4. use the data analysis tools to analyze the trace data.
//!
//! You could use the `trace-cmd` to trace the events, and use the `kernelshark` to analyze the trace data.
//!
//! # Note
//!
//! - You could resize the buffer size of the trace file in `/sys/kernel/debug/tracing/buffer_size_kb` if the trace file is too large.
//!
//! ```
//! echo 1024 > /sys/kernel/debug/tracing/buffer_size_kb
//! ```

use core::ops::Deref;

use kernel::bindings;
use kernel::c_types::c_void;
use kernel::ktime;
use kernel::str::CStr;

use crate::clock::RrosClock;
use crate::timer::RrosTimer;
use crate::wait::RrosWaitChannel;
use crate::{
    sched::{rros_rq, RrosInitThreadAttr, RrosThread},
    thread::rros_get_inband_pid,
};

fn slice_to_iovec(array: &[u8]) -> bindings::iovec {
    bindings::iovec {
        iov_base: array.as_ptr() as *const _ as *mut c_void,
        iov_len: array.len() as u64,
    }
}

fn empty_iovec() -> bindings::iovec {
    bindings::iovec {
        iov_base: core::ptr::null_mut(),
        iov_len: 0,
    }
}

pub fn trace_rros_schedule(rq: &rros_rq) {
    extern "C" {
        fn rust_helper_trace_rros_schedule(flags: u64, local_flags: u64);
    }
    let flags = rq.flags;
    let _local_flags = rq.flags;
    unsafe {
        rust_helper_trace_rros_schedule(flags, rq.flags);
    }
}
pub fn trace_rros_reschedule_ipi(rq: &rros_rq) {
    extern "C" {
        fn rust_helper_trace_rros_reschedule_ipi(flags: u64, local_flags: u64);
    }
    let flags = rq.flags;
    let local_flags = rq.flags;
    unsafe {
        rust_helper_trace_rros_reschedule_ipi(flags, local_flags);
    }
}
pub fn trace_rros_pick_thread(next: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_pick_thread(name: bindings::iovec, next_pid: i32);
    }
    let _thread_name = [0u8; 32];
    let next_pid = rros_get_inband_pid(next as *const RrosThread);
    unsafe {
        let array = next.name.as_bytes();
        let iov = slice_to_iovec(array);
        rust_helper_trace_rros_pick_thread(iov, next_pid);
    }
}

pub fn trace_rros_switch_context(prev: &RrosThread, next: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_switch_context(
            prev_name: bindings::iovec,
            next_name: bindings::iovec,
            prev_pid: u32,
            prev_prio: i32,
            prev_state: u32,
            next_pid: u32,
            next_prio: i32,
        );
    }

    let _prev_thread_name = [0u8; 32];
    let _next_thread_name = [0u8; 32];

    let prev_array = prev.name.as_bytes();
    let next_array = next.name.as_bytes();
    let prev_iov = slice_to_iovec(prev_array);
    let next_iov = slice_to_iovec(next_array);

    let prev_pid = rros_get_inband_pid(prev as *const RrosThread);
    let next_pid = rros_get_inband_pid(next as *const RrosThread);

    let prev_prio = prev.cprio;
    let next_prio = next.cprio;

    let prev_state = prev.state;

    unsafe {
        rust_helper_trace_rros_switch_context(
            prev_iov,
            next_iov,
            prev_pid as u32,
            prev_prio,
            prev_state,
            next_pid as u32,
            next_prio,
        );
    }
}

// TODO: finish_rq_switch is not impl.
pub fn trace_rros_switch_tail(curr: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_switch_tail(curr_name: bindings::iovec, curr_pid: u32);
    }
    let curr_pid = rros_get_inband_pid(curr);
    let curr_array = curr.name.as_bytes();
    let curr_iov = slice_to_iovec(curr_array);

    unsafe {
        rust_helper_trace_rros_switch_tail(curr_iov, curr_pid as u32);
    }
}
pub fn trace_rros_init_thread(thread: &RrosThread, iattr: &RrosInitThreadAttr, status: i32) {
    extern "C" {
        fn rust_helper_trace_rros_init_thread(
            thread: *mut c_void,
            thread_name: bindings::iovec,
            class_name: bindings::iovec,
            flags: u64,
            cprio: i32,
            status: i32,
        );
    }

    let _thread_name = [0u8; 32];
    let _class_name = [0u8; 32];

    let thread_name_array = thread.name.as_bytes();
    let class_name_array = iattr.sched_class.unwrap().name.as_bytes();
    let thread_iov = slice_to_iovec(thread_name_array);
    let class_iov = slice_to_iovec(class_name_array);

    let flags = iattr.flags;
    let cprio = thread.cprio;
    unsafe {
        rust_helper_trace_rros_init_thread(
            thread as *const _ as *mut c_void,
            thread_iov,
            class_iov,
            flags as u64,
            cprio,
            status as i32,
        );
    }
}
// NOTE: RrosClock.name is private, so add `get_name()` for RrosClock.
// TODO: rros_sleep_on_locked is not impl. We use the unsafe code `evl_delay`.
pub fn trace_rros_sleep_on(
    thread: &RrosThread,
    timeout: ktime::KtimeT,
    timeout_mode: i32,
    wchan: *mut RrosWaitChannel,
    clock: &RrosClock,
) {
    extern "C" {
        fn rust_helper_trace_rros_sleep_on(
            pid: u32,
            timeout: bindings::ktime_t,
            timeout_mode: i32,
            wchan: *mut c_void,
            clock_name: bindings::iovec,
            wchan_name: bindings::iovec,
        );
    }
    let clock_name = clock.get_name();
    let clock_array = clock_name.as_bytes();
    let clock_iov = slice_to_iovec(clock_array);
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    // TODO: unknown fileds `wchan_name`
    let wchan_name = slice_to_iovec("unknown fileds".as_bytes());
    unsafe {
        rust_helper_trace_rros_sleep_on(
            pid as u32,
            timeout,
            timeout_mode,
            wchan as *mut _ as *mut c_void,
            clock_iov,
            wchan_name,
        );
    }
}
pub fn trace_rros_wakeup_thread(thread: &RrosThread, mask: u32, _info: i32) {
    extern "C" {
        fn rust_helper_trace_rros_wakeup_thread(
            thread_name: bindings::iovec,
            pid: u32,
            mask: i32,
            info: i32,
        );
    }
    let thread_array = thread.name.as_bytes();
    let thread_iov = slice_to_iovec(thread_array);
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let info = thread.info;

    unsafe {
        rust_helper_trace_rros_wakeup_thread(thread_iov, pid as u32, mask as i32, info as i32);
    }
}
pub fn trace_rros_hold_thread(thread: &RrosThread, mask: u32) {
    extern "C" {
        fn rust_helper_trace_rros_hold_thread(thread_name: bindings::iovec, pid: u32, mask: u64);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let thread_array = thread.name.as_bytes();
    let thread_iovec = slice_to_iovec(thread_array);
    unsafe {
        rust_helper_trace_rros_hold_thread(thread_iovec, pid as u32, mask as u64);
    }
}
pub fn trace_rros_release_thread(thread: &RrosThread, mask: u32, info: u32) {
    extern "C" {
        fn rust_helper_trace_rros_release_thread(
            thread_name: bindings::iovec,
            pid: u32,
            mask: i32,
            info: i32,
        );
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let thread_array = thread.name.as_bytes();
    let thread_iovec = slice_to_iovec(thread_array);
    unsafe {
        rust_helper_trace_rros_release_thread(thread_iovec, pid as u32, mask as i32, info as i32);
    }
}
pub fn trace_rros_thread_set_current_prio(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_set_current_prio(
            thread: *mut c_void,
            pid: u32,
            cprio: i32,
        );
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let cprio = thread.cprio;
    unsafe {
        rust_helper_trace_rros_thread_set_current_prio(
            thread as *const _ as *mut c_void,
            pid as u32,
            cprio,
        );
    }
}
pub fn trace_rros_thread_cancel(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_cancel(pid: u32, state: u32, info: u32);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_thread_cancel(pid as u32, state, info);
    }
}
//TODO: rros_join_thread is not impl.
pub fn trace_rros_thread_join(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_join(pid: i32, state: u32, info: u32);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_thread_join(pid, state, info);
    }
}
//TODO: rros_unblock_thread is not impl.
pub fn trace_rros_unblock_thread(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_unblock_thread(pid: i32, state: u32, info: u32);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_unblock_thread(pid, state, info);
    }
}
pub fn trace_rros_thread_wait_period(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_wait_period(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_thread_wait_period(state, info);
    }
}
pub fn trace_rros_thread_missed_period(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_missed_period(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_thread_missed_period(state, info);
    }
}
// TODO: func rros_migrate_thread is not impl.
pub fn trace_rros_thread_migrate(thread: &RrosThread, cpu: u32) {
    extern "C" {
        fn rust_helper_trace_rros_thread_migrate(thread: *mut c_void, pid: u32, cpu: u32);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    unsafe {
        rust_helper_trace_rros_thread_migrate(thread as *const _ as *mut c_void, pid as u32, cpu);
    }
}
pub fn trace_rros_watchdog_signal(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_watchdog_signal(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_watchdog_signal(state, info);
    }
}
pub fn trace_rros_switch_oob(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_switch_oob(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_switch_oob(state, info);
    }
}
pub fn trace_rros_switched_oob(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_switched_oob(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_switched_oob(state, info);
    }
}
pub fn trace_rros_switch_inband(cause: i32) {
    extern "C" {
        fn rust_helper_trace_rros_switch_inband(cause: i32);
    }
    unsafe {
        rust_helper_trace_rros_switch_inband(cause);
    }
}
pub fn trace_rros_switched_inband(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_switched_inband(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_switched_inband(state, info);
    }
}
pub fn trace_rros_kthread_entry(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_kthread_entry(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_kthread_entry(state, info);
    }
}
pub fn trace_rros_thread_map(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_map(thread: *mut c_void, pid: u32, prio: i32);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    let prio = thread.bprio;
    unsafe {
        rust_helper_trace_rros_thread_map(thread as *const _ as *mut c_void, pid as u32, prio);
    }
}
pub fn trace_rros_thread_unmap(thread: &RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_thread_unmap(state: u32, info: u32);
    }
    let state = thread.state;
    let info = thread.info;
    unsafe {
        rust_helper_trace_rros_thread_unmap(state, info);
    }
}
pub fn trace_rros_inband_wakeup(thread: *const RrosThread) {
    extern "C" {
        fn rust_helper_trace_rros_inband_wakeup(pid: u32, comm: bindings::iovec);
    }
    unsafe {
        let task = (*thread).altsched.0.task;
        let pid = (*task).pid;
        let comm_array = (*task).comm;
        let comm = bindings::iovec {
            iov_base: comm_array.as_ptr() as *const _ as *mut c_void,
            iov_len: comm_array.len() as u64,
        };
        rust_helper_trace_rros_inband_wakeup(pid as u32, comm);
    }
}

pub fn trace_rros_inband_signal(thread: &RrosThread, sig: i32, sigval: i32) {
    extern "C" {
        fn rust_helper_trace_rros_inband_signal(
            element_name: bindings::iovec,
            pid: u32,
            sig: i32,
            sigval: i32,
        );
    }
    let element = thread.element.clone();
    let devname = element.deref().borrow();
    let devname = devname.deref().devname.as_ref();
    let element_name_iovec;
    if devname.is_none() {
        element_name_iovec = empty_iovec();
    } else {
        let element_name_array =
            unsafe { CStr::from_char_ptr(devname.unwrap().get_name()).as_bytes() };
        element_name_iovec = slice_to_iovec(element_name_array);
    }
    let pid = rros_get_inband_pid(thread as *const RrosThread);
    unsafe {
        rust_helper_trace_rros_inband_signal(element_name_iovec, pid as u32, sig, sigval);
    }
}

// NOTE: `__rros_stop_timer` seems incorrect.
pub fn trace_rros_timer_stop(timer: &RrosTimer) {
    extern "C" {
        fn rust_helper_trace_rros_timer_stop(name: bindings::iovec);
    }
    let name_array = timer.get_name().as_bytes();
    let name_iov = slice_to_iovec(name_array);
    unsafe {
        rust_helper_trace_rros_timer_stop(name_iov);
    }
}

pub fn trace_rros_timer_expire(timer: &RrosTimer) {
    extern "C" {
        fn rust_helper_trace_rros_timer_expire(name: bindings::iovec);
    }
    let name_array = timer.get_name().as_bytes();
    let name_iov = slice_to_iovec(name_array);
    unsafe {
        rust_helper_trace_rros_timer_expire(name_iov);
    }
}
pub fn trace_rros_timer_start(timer: &RrosTimer, value: ktime::KtimeT, interval: ktime::KtimeT) {
    extern "C" {
        fn rust_helper_trace_rros_timer_start(
            timer_name: bindings::iovec,
            value: bindings::ktime_t,
            interval: bindings::ktime_t,
        );
    }
    let name_array = timer.get_name().as_bytes();
    let name_iov = slice_to_iovec(name_array);
    unsafe {
        rust_helper_trace_rros_timer_start(name_iov, value, interval);
    }
}

pub fn trace_rros_timer_move(timer: &RrosTimer, clock: &RrosClock, cpu: u32) {
    extern "C" {
        fn rust_helper_trace_rros_timer_move(
            timer_name: bindings::iovec,
            clock_name: bindings::iovec,
            cpu: u32,
        );
    }
    let timer_name_array = timer.get_name().as_bytes();
    let timer_name_iov = slice_to_iovec(timer_name_array);
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_timer_move(timer_name_iov, clock_name_iov, cpu);
    }
}
pub fn trace_rros_timer_shot(timer: &RrosTimer, delta: i64, cycles: i64) {
    extern "C" {
        fn rust_helper_trace_rros_timer_shot(timer_name: bindings::iovec, delta: i64, cycles: u64);
    }
    let name_array = timer.get_name().as_bytes();
    let name_iov = slice_to_iovec(name_array);
    unsafe {
        rust_helper_trace_rros_timer_shot(name_iov, delta, cycles as u64);
    }
}
// NOTE: trace in `RrosWaitQueue.locked_add`
// FIXME: wq.wchan(RrosWaitChannel) doesn't have the filed named `name`
// pub fn trace_rros_wait(wq: &RrosWaitQueue){
//     extern "C"{
//         fn rust_helper_trace_rros_wait(name:bindings::iovec);
//     }
//     unsafe{
//         rust_helper_trace_rros_wait(name);
//     }
// }
// NOTE: trace in `RrosWaitQueue.wake_up`
// FIXME: wq.wchan(RrosWaitChannel) doesn't have the filed named `name`
// pub fn trace_rros_wake_up(wq: &RrosWaitQueue){
//     extern "C"{
//         fn rust_helper_trace_rros_wake_up(name:bindings::iovec);
//     }
//     unsafe{
//         rust_helper_trace_rros_wake_up(name);
//     }
// }
// NOTE: trace in `RrosWaitQueue.flush_locked`
// FIXME: wq.wchan(RrosWaitChannel) doesn't have the filed named `name`
// pub fn trace_rros_flush_wait(wq: &RrosWaitQueue){
//     extern "C"{
//         fn rust_helper_trace_rros_flush_wait(name:bindings::iovec);
//     }
//     unsafe{
//         rust_helper_trace_rros_flush_wait(name);
//     }
// }
// NOTE: trace in `RrosWaitQueue.wait_schedule`
// FIXME: wq.wchan(RrosWaitChannel) doesn't have the filed named `name`
// pub fn trace_rros_finish_wait(wq: &RrosWaitQueue){
//     extern "C"{
//         fn rust_helper_trace_rros_finish_wait(name:bindings::iovec);
//     }
//     unsafe{
//         rust_helper_trace_rros_finish_wait(name);
//     }
// }
pub fn trace_rros_oob_sysentry(nr: u32) {
    extern "C" {
        fn rust_helper_trace_rros_oob_sysentry(nr: u32);
    }
    unsafe {
        rust_helper_trace_rros_oob_sysentry(nr);
    }
}
pub fn trace_rros_oob_sysexit(result: i64) {
    extern "C" {
        fn rust_helper_trace_rros_oob_sysexit(result: i64);
    }
    unsafe {
        rust_helper_trace_rros_oob_sysexit(result);
    }
}
pub fn trace_rros_inband_sysentry(nr: u32) {
    extern "C" {
        fn rust_helper_trace_rros_inband_sysentry(nr: u32);
    }
    unsafe {
        rust_helper_trace_rros_inband_sysentry(nr);
    }
}
pub fn trace_rros_inband_sysexit(result: i64) {
    extern "C" {
        fn rust_helper_trace_rros_inband_sysexit(result: i64);
    }
    unsafe {
        rust_helper_trace_rros_inband_sysexit(result);
    }
}

pub fn trace_rros_thread_update_mode(thread: &RrosThread, mode: i32, set: bool) {
    extern "C" {
        fn rust_helper_trace_rros_thread_update_mode(
            element_name: bindings::iovec,
            mode: i32,
            set: bool,
        );
    }
    let element = thread.element.clone();
    let devname = element.deref().borrow();
    let devname = devname.deref().devname.as_ref();
    let element_name_iovec;
    if devname.is_none() {
        element_name_iovec = empty_iovec();
    } else {
        let element_name_array =
            unsafe { CStr::from_char_ptr(devname.unwrap().get_name()).as_bytes() };
        element_name_iovec = slice_to_iovec(element_name_array);
    }
    unsafe {
        rust_helper_trace_rros_thread_update_mode(element_name_iovec, mode, set);
    }
}

pub fn trace_rros_clock_getres(clock: &RrosClock, val: *const bindings::timespec64) {
    extern "C" {
        fn rust_helper_trace_rros_clock_getres(
            clock_name: bindings::iovec,
            val: *const bindings::timespec64,
        );
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_clock_getres(clock_name_iov, val);
    }
}

pub fn trace_rros_clock_gettime(clock: &RrosClock, val: *const bindings::timespec64) {
    extern "C" {
        fn rust_helper_trace_rros_clock_gettime(
            clock_name: bindings::iovec,
            val: *const bindings::timespec64,
        );
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_clock_gettime(clock_name_iov, val);
    }
}

pub fn trace_rros_clock_settime(clock: &RrosClock, val: *const bindings::timespec64) {
    extern "C" {
        fn rust_helper_trace_rros_clock_settime(
            clock_name: bindings::iovec,
            val: *const bindings::timespec64,
        );
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_clock_settime(clock_name_iov, val);
    }
}
pub fn trace_rros_clock_adjtime(clock: &RrosClock, tx: *mut bindings::__kernel_timex) {
    extern "C" {
        fn rust_helper_trace_rros_clock_adjtime(
            clock_name: bindings::iovec,
            tx: *mut bindings::__kernel_timex,
        );
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_clock_adjtime(clock_name_iov, tx);
    }
}

pub fn trace_rros_register_clock(clock: &RrosClock) {
    extern "C" {
        fn rust_helper_trace_rros_register_clock(name: bindings::iovec);
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_register_clock(clock_name_iov);
    }
}

pub fn trace_rros_unregister_clock(clock: &RrosClock) {
    extern "C" {
        fn rust_helper_trace_rros_unregister_clock(name: bindings::iovec);
    }
    let clock_name_array = clock.get_name().as_bytes();
    let clock_name_iov = slice_to_iovec(clock_name_array);
    unsafe {
        rust_helper_trace_rros_unregister_clock(clock_name_iov);
    }
}

pub fn trace_rros_trace(msg: &[u8]) {
    extern "C" {
        fn rust_helper_trace_rros_trace(msg: bindings::iovec);
    }
    let msg_iov = slice_to_iovec(msg);
    unsafe {
        rust_helper_trace_rros_trace(msg_iov);
    }
}
