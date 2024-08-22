use core::{
    cell::UnsafeCell,
    mem::size_of,
    ptr::null_mut,
    result::Result::Err,
    sync::atomic::{fence, Ordering},
};

use alloc::sync::Arc;

use crate::{
    arch::arm64::fptest::{rros_begin_fpu, rros_detect_fpu},
    clock::RROS_MONO_CLOCK,
    factory::RROS_CLONE_PUBLIC,
    file::{rros_open_file, rros_release_file, RrosFile},
    flags::RrosFlag,
    guard::Guard,
    stax::Stax,
    thread::{rros_kthread_should_stop, rros_run_kthread_on_cpu, rros_stop_kthread, RrosKthread},
    timeout::RROS_INFINITE,
    timer::*,
};

use kernel::{
    bindings, c_str, c_types, chrdev, class, container_of,
    cpumask::{num_online_cpus, CpumaskT},
    device, file_operations,
    io_buffer::{IoBufferReader, IoBufferWriter, ReadableFromBytes, WritableToBytes},
    irq_work::IrqWork,
    ktime::KtimeT,
    mutex_init,
    prelude::*,
    spinlock_init,
    str::CStr,
    sync::{Lock, Mutex, Semaphore, SpinLock},
    task::Task,
    user_ptr::UserSlicePtr,
    KernelModule,
};

/* hectic_task_index.flags */
const HECTIC_OOB_WAIT: u32 = 0x10000;
const HECTIC_INBAND_WAIT: u32 = 0;

const RROS_HECTIC_IOCBASE: u32 = 'H' as u32;

const RROS_HECIOC_SET_TASKS_COUNT: u32 = kernel::ioctl::_IOW::<u32>(RROS_HECTIC_IOCBASE, 0);
const RROS_HECIOC_SET_CPU: u32 = kernel::ioctl::_IOW::<u32>(RROS_HECTIC_IOCBASE, 1);
const RROS_HECIOC_REGISTER_UTASK: u32 =
    kernel::ioctl::_IOW::<HecticTaskIndex>(RROS_HECTIC_IOCBASE, 2);
const RROS_HECIOC_CREATE_KTASK: u32 =
    kernel::ioctl::_IOWR::<HecticTaskIndex>(RROS_HECTIC_IOCBASE, 3);
const RROS_HECIOC_PEND: u32 = kernel::ioctl::_IOR::<HecticTaskIndex>(RROS_HECTIC_IOCBASE, 4);
const RROS_HECIOC_SWITCH_TO: u32 = kernel::ioctl::_IOR::<HecticSwitchReq>(RROS_HECTIC_IOCBASE, 5);
const RROS_HECIOC_GET_SWITCHES_COUNT: u32 = kernel::ioctl::_IOR::<u32>(RROS_HECTIC_IOCBASE, 6);
const RROS_HECIOC_GET_LAST_ERROR: u32 = kernel::ioctl::_IOR::<HecticError>(RROS_HECTIC_IOCBASE, 7);
const RROS_HECIOC_SET_PAUSE: u32 = kernel::ioctl::_IOW::<u32>(RROS_HECTIC_IOCBASE, 8);
const RROS_HECIOC_LOCK_STAX: u32 = 18441;
const RROS_HECIOC_UNLOCK_STAX: u32 = 18442;

const HECTIC_KTHREAD: u32 = 0x20000;

static mut FP_FEATURES: u32 = 0;

module! {
    type: Hecticdev,
    name: b"hectic",
    author: b"wxg",
    description: b"hectic driver",
    license: b"GPL v2",
}

#[repr(C)]
struct HecticTaskIndex {
    index: u32,
    flags: u32,
}

unsafe impl ReadableFromBytes for HecticTaskIndex {}
unsafe impl WritableToBytes for HecticTaskIndex {}

#[repr(C)]
struct HecticSwitchReq {
    from: u32,
    to: u32,
}

unsafe impl ReadableFromBytes for HecticSwitchReq {}

#[repr(C)]
struct HecticError {
    last_switch: HecticSwitchReq,
    fp_val: u32,
}

unsafe impl WritableToBytes for HecticError {}

struct RtswitchTask {
    base: HecticTaskIndex,
    rt_synch: RrosFlag,
    // FIXME: To be replaced with other synchronization primitives when upgrading to the latest version of RFL.
    nrt_synch: Semaphore,
    kthread: RrosKthread,
    last_switch: u32,
    #[allow(dead_code)]
    ctx: *mut RtswitchContext,
}

pub struct RtswitchContext {
    tasks: Vec<Box<RtswitchTask>>,
    tasks_count: u32,
    next_index: u32,
    lock: Mutex<()>,
    cpu: u32,
    switches_count: u32,
    pause_us: u64,
    next_task: u32,
    wake_up_delay: Arc<SpinLock<RrosTimer>>,
    failed: bool,
    error: HecticError,
    utask: u32,
    wake_utask: IrqWork,
    stax: Pin<Box<Stax<()>>>,
    o_guard: SpinLock<Vec<usize>>,
    i_guard: SpinLock<Vec<usize>>,
    rfile: RrosFile,
}

impl RtswitchTask {
    fn new() -> Self {
        Self {
            base: HecticTaskIndex { index: 0, flags: 0 },
            rt_synch: RrosFlag::new(),
            nrt_synch: Semaphore::new(),
            kthread: RrosKthread::new(None),
            last_switch: 0,
            ctx: null_mut(),
        }
    }
}

impl RtswitchContext {
    fn new() -> Result<Self> {
        // Spinlock and Stax have to be initialized after new.So their new function is unsafe.
        // Safety: We promise that the SpinLock and Stax will be initialized before they are used.
        let ctx = RtswitchContext {
            tasks: Vec::new(),
            tasks_count: 0,
            next_index: 0,
            lock: unsafe { Mutex::new(()) },
            cpu: 0,
            switches_count: 0,
            pause_us: 0,
            next_task: 0,
            wake_up_delay: Arc::try_new(unsafe { SpinLock::new(RrosTimer::new(0)) })?,
            failed: false,
            error: HecticError {
                last_switch: HecticSwitchReq {
                    from: u32::MAX,
                    to: u32::MAX,
                },
                fp_val: 0,
            },
            utask: u32::MAX,
            wake_utask: IrqWork::new(),
            stax: unsafe { Pin::from(Box::try_new(Stax::new(()))?) },
            o_guard: unsafe { SpinLock::new(Vec::new()) },
            i_guard: unsafe { SpinLock::new(Vec::new()) },
            rfile: RrosFile::new(),
        };
        Ok(ctx)
    }

    fn init(&mut self) -> Result<()> {
        let l_pinned = unsafe { Pin::new_unchecked(&mut self.lock) };
        mutex_init!(l_pinned, "rtswitch_ctx_lock");
        self.wake_utask.init_irq_work(rtswitch_utask_waker)?;

        // FIXME: to be replaced with `rros_init_timer`
        unsafe {
            let this = self as *mut RtswitchContext;
            let timer;
            if let Some(t) = Arc::get_mut(&mut self.wake_up_delay) {
                timer = t;
            } else {
                return Err(Error::EINVAL);
            }
            (*timer.locked_data().get()).pointer = this as *mut u8;
            let t_pinned = Pin::new_unchecked(timer);
            spinlock_init!(t_pinned, "wake_up_delay");

            rros_init_timer_on_rq(
                self.wake_up_delay.clone(),
                &mut RROS_MONO_CLOCK,
                Some(timed_wake_up),
                null_mut(),
                c_str!("timed_wake_up"),
                RROS_TIMER_IGRAVITY,
            );
        }

        Stax::init((&mut self.stax).as_mut())?;
        let o_pinned = unsafe { Pin::new_unchecked(&mut self.o_guard) };
        spinlock_init!(o_pinned, "o_guard");
        let i_pinned = unsafe { Pin::new_unchecked(&mut self.i_guard) };
        spinlock_init!(i_pinned, "i_guard");

        Ok(())
    }

    #[allow(dead_code)]
    fn handle_fpu_error(&mut self, _fp_in: u32, _fp_out: u32, _bad_reg: i32) {}

    fn rtswitch_pend_rt(&mut self, idx: u32) -> Result<i32> {
        let task: &mut RtswitchTask;
        let rc: i32;

        if idx > self.tasks_count {
            return Err(Error::EINVAL);
        }

        task = &mut self.tasks[idx as usize];
        task.base.flags |= HECTIC_OOB_WAIT;

        rc = task.rt_synch.wait();
        if rc < 0 {
            return Err(Error::from_kernel_errno(rc));
        }

        if self.failed {
            return Ok(1);
        }

        Ok(0)
    }

    fn rtswitch_to_rt(&mut self, from_idx: u32, mut to_idx: u32) -> Result<i32> {
        let rc: i32;

        if from_idx > self.tasks_count || to_idx > self.tasks_count {
            return Err(Error::EINVAL);
        }

        /* to == from is a special case which means
        "return to the previous task". */
        if to_idx == from_idx {
            to_idx = self.error.last_switch.from;
        }

        let (from, to) = vec_get_mut(&mut self.tasks, from_idx as usize, to_idx as usize);

        from.base.flags |= HECTIC_OOB_WAIT;
        self.switches_count += 1;
        from.last_switch = self.switches_count;
        self.error.last_switch.from = from_idx;
        self.error.last_switch.to = to_idx;
        fence(Ordering::SeqCst);

        if self.pause_us > 0 {
            self.next_task = to_idx;
            fence(Ordering::SeqCst);
            rros_start_timer(
                self.wake_up_delay.clone(),
                rros_abs_timeout(self.wake_up_delay.clone(), (self.pause_us * 1000) as KtimeT),
                RROS_INFINITE,
            );
        } else {
            match to.base.flags & HECTIC_OOB_WAIT {
                HECTIC_INBAND_WAIT => {
                    self.utask = to_idx;
                    fence(Ordering::SeqCst);
                    self.wake_utask.irq_work_queue()?;
                }
                HECTIC_OOB_WAIT => {
                    to.rt_synch.raise();
                }
                _ => {
                    return Err(Error::EINVAL);
                }
            }
        }

        rc = from.rt_synch.wait();
        if rc < 0 {
            return Err(Error::from_kernel_errno(rc));
        }

        if self.failed {
            return Ok(1);
        }

        Ok(0)
    }

    fn rtswitch_pend_nrt(&mut self, idx: u32) -> Result<i32> {
        let task: &mut RtswitchTask;

        if idx >= self.tasks_count {
            return Err(Error::EINVAL);
        }

        task = &mut self.tasks[idx as usize];
        task.base.flags &= !HECTIC_OOB_WAIT;

        task.nrt_synch.down_interruptible()?;

        if self.failed {
            return Ok(1);
        }
        Ok(0)
    }

    fn rtswitch_to_nrt(&mut self, from_idx: u32, mut to_idx: u32) -> Result<i32> {
        let fp_check: bool;
        // TODO: Some unused variables related to FPU check
        let mut _expected: u32;
        let mut _fp_val: u32;
        let mut _bad_reg: i32;

        if from_idx > self.tasks_count || to_idx > self.tasks_count {
            return Err(Error::EINVAL);
        }

        /* to == from is a special case which means
        "return to the previous task". */
        if to_idx == from_idx {
            to_idx = self.error.last_switch.from;
        }

        let (from, to) = vec_get_mut(&mut self.tasks, from_idx as usize, to_idx as usize);

        fp_check = (self.switches_count == from.last_switch + 1)
            && (self.error.last_switch.from == to_idx)
            && (self.error.last_switch.to == from_idx);

        from.base.flags &= !HECTIC_OOB_WAIT;
        self.switches_count += 1;
        from.last_switch = self.switches_count;
        self.error.last_switch.from = from_idx;
        self.error.last_switch.to = to_idx;
        fence(Ordering::SeqCst);

        if self.pause_us > 0 {
            self.next_task = to_idx;
            fence(Ordering::SeqCst);
            rros_start_timer(
                self.wake_up_delay.clone(),
                rros_abs_timeout(self.wake_up_delay.clone(), (self.pause_us * 1000) as KtimeT),
                RROS_INFINITE,
            );
        } else {
            match to.base.flags & HECTIC_OOB_WAIT {
                HECTIC_INBAND_WAIT => {
                    to.nrt_synch.up();
                }
                HECTIC_OOB_WAIT if fp_check && rros_begin_fpu() => {
                    // TODO: On arm64, rros_begin_fpu() returns false. Complete this section when RROS supports additional architectures in the future.
                }
                HECTIC_OOB_WAIT => {
                    to.rt_synch.raise();
                }
                _ => {
                    return Err(Error::EINVAL);
                }
            }
        }

        from.nrt_synch.down_interruptible()?;

        if self.failed {
            return Ok(1);
        }
        Ok(0)
    }

    fn rtswitch_set_tasks_count(&mut self, count: u32) -> Result<i32> {
        if self.tasks_count == count {
            return Ok(0);
        }

        let mut tasks = Vec::new();
        for _ in 0..count {
            let task = Box::try_new(RtswitchTask::new())?;
            tasks.try_push(task)?;
        }

        let _guard = self.lock.lock();
        self.tasks = tasks;
        self.tasks_count = count;
        self.next_index = 0;

        Ok(0)
    }

    fn rtswitch_register_task(&mut self, arg: &mut HecticTaskIndex, flags: i32) -> Result<i32> {
        let t: &mut RtswitchTask;

        let _guard = self.lock.lock();

        if self.next_index == self.tasks_count {
            return Err(Error::EBUSY);
        }

        arg.index = self.next_index;
        t = &mut self.tasks[arg.index as usize];
        self.next_index += 1;
        t.base.index = arg.index;
        t.base.flags = (arg.flags & HECTIC_OOB_WAIT) | flags as u32;
        t.last_switch = 0;
        t.nrt_synch.init(0);
        t.rt_synch.init();

        Ok(0)
    }

    fn rtswitch_create_kthread(&mut self, ptask: &mut HecticTaskIndex) -> Result<i32> {
        let task: &mut RtswitchTask;
        let fmt: &'static CStr;
        let res: Result<i32>;

        let this = self as *mut RtswitchContext;
        self.rtswitch_register_task(ptask, HECTIC_KTHREAD as i32)?;
        task = &mut self.tasks[ptask.index as usize];
        task.ctx = this;

        let i = ptask.index;
        let kthread_fn = Box::try_new(move || {
            rtswitch_kthread(this, i);
        })?;

        // TODO: ptask.index, self.cpu, Task::current().pid())
        fmt = c_str!("rtk%d@%u:%d");

        res = rros_run_kthread_on_cpu(
            &mut task.kthread,
            self.cpu,
            kthread_fn,
            RROS_CLONE_PUBLIC,
            fmt,
        )
        .map(|r| r as i32);
        if res.is_err() {
            task.base.flags = 0;
        }

        res
    }
}

// TODO:
// Refactoring needed to remove unsafe code and pass `RtswitchTask` as a parameter.
// This should be done after the `RrosKthread` abstraction refactoring is complete.
fn rtswitch_kthread(ctx: *mut RtswitchContext, idx: u32) {
    let mut to: u32 = idx;
    let mut i: u32 = 0;

    let ctx = unsafe { &mut *ctx };

    if let Err(_) = ctx.rtswitch_pend_rt(idx) {
        pr_err!("rtswitch_kthread: rtswitch_pend_rt failed\n");
    };

    while !rros_kthread_should_stop() {
        match i % 3 {
            0 => {
                /* to == from means "return to last task" */
                if let Err(_) = ctx.rtswitch_to_rt(idx, idx) {
                    pr_err!("rtswitch_kthread: rtswitch_to_rt failed\n");
                };
            }
            1 => {
                to += 1;
                if to == idx {
                    to += 1;
                }
                if to > ctx.tasks_count - 1 {
                    to = 0;
                }
                if to == idx {
                    to += 1;
                }
                if let Err(_) = ctx.rtswitch_to_rt(idx, to) {
                    pr_err!("rtswitch_kthread: rtswitch_to_rt failed\n");
                };
            }
            2 => {
                if let Err(_) = ctx.rtswitch_to_rt(idx, to) {
                    pr_err!("rtswitch_kthread: rtswitch_to_rt failed\n");
                };
            }
            _ => {
                pr_err!("rtswitch_kthread: unexpected task index\n");
            }
        }
        i += 1;
        if i == 4000000 {
            i = 0;
        }
    }
}

fn timed_wake_up(timer: *mut RrosTimer) {
    let ctx = unsafe { &mut *((*timer).pointer as *mut RtswitchContext) };
    let task: &mut RtswitchTask = &mut ctx.tasks[ctx.next_task as usize];

    match task.base.flags & HECTIC_OOB_WAIT {
        HECTIC_INBAND_WAIT => {
            ctx.utask = ctx.next_task;
            match ctx.wake_utask.irq_work_queue() {
                Ok(_) => {}
                Err(_) => {
                    pr_err!("timed_wake_up: irq_work_queue failed\n");
                }
            };
        }
        HECTIC_OOB_WAIT => {
            task.rt_synch.raise();
        }
        _ => {
            pr_err!("timed_wake_up: unexpected task flags\n");
        }
    }
}

unsafe extern "C" fn rtswitch_utask_waker(work: *mut IrqWork) {
    let ctx =
        unsafe { &mut *(container_of!(work, RtswitchContext, wake_utask) as *mut RtswitchContext) };
    let task: &mut RtswitchTask = &mut ctx.tasks[ctx.utask as usize];
    task.nrt_synch.up();
}

/// Returns mutable references to two elements in the vector by using split_at_mut.
fn vec_get_mut<T>(v: &mut Vec<T>, from_idx: usize, to_idx: usize) -> (&mut T, &mut T) {
    if from_idx < to_idx {
        let (left, right) = v.split_at_mut(to_idx);
        (&mut left[from_idx], &mut right[0])
    } else {
        let (left, right) = v.split_at_mut(from_idx);
        (&mut right[0], &mut left[to_idx])
    }
}

pub struct HecticFile;

impl file_operations::FileOpener<u8> for HecticFile {
    fn open(_context: &u8, file: &kernel::file::File) -> kernel::Result<Self::Wrapper> {
        let mut ctx: Box<UnsafeCell<RtswitchContext>> =
            Box::try_new(UnsafeCell::new(RtswitchContext::new()?))?;
        let ctx_ref = ctx.as_mut().get_mut();
        ctx_ref.init()?;
        let rfile = &mut ctx_ref.rfile;
        rros_open_file(rfile, file.get_ptr())?;
        Ok(ctx)
    }
}

fn lock_stax(ctx: &mut RtswitchContext, is_inband: bool) -> Result<i32> {
    match ctx.stax.as_ref().get_ref().lock() {
        Ok(g) => {
            let tmp = Box::try_new(g)?;
            let mut guard = if is_inband {
                ctx.i_guard.lock()
            } else {
                ctx.o_guard.lock()
            };
            guard.try_push(Box::into_raw(tmp) as usize)?;
            Ok(0)
        }
        Err(e) => {
            return Err(e);
        }
    }
}

fn unlock_stax(ctx: &mut RtswitchContext, is_inband: bool) -> Result<i32> {
    let tmp;
    let mut count = 0;
    loop {
        count += 1;
        let mut guard = if is_inband {
            ctx.i_guard.lock()
        } else {
            ctx.o_guard.lock()
        };
        if let Some(t) = guard.pop() {
            tmp = t;
            break;
        }
        if count > 1000 {
            return Ok(0);
        }
    }
    // Safety: Every pointer will be pushed and poped in a pair. And the pointer is vaild before it is poped.
    unsafe {
        Box::from_raw(tmp as *mut Guard<'_, Stax<()>>);
    }
    Ok(0)
}

impl file_operations::FileOperations for HecticFile {
    kernel::declare_file_operations!(ioctl, oob_ioctl, compat_ioctl, compat_oob_ioctl);

    type Wrapper = Box<UnsafeCell<RtswitchContext>>;

    fn release(mut ctx: Self::Wrapper, _file: &kernel::file::File) {
        let ctx_ref = ctx.as_mut().get_mut();
        rros_destroy_timer(ctx_ref.wake_up_delay.clone());

        if !ctx_ref.tasks.is_empty() {
            // FIXME: To be wrapped
            unsafe {
                bindings::set_cpus_allowed_ptr(
                    Task::current_ptr(),
                    CpumaskT::cpumask_of(ctx_ref.cpu) as *const _,
                );
            }

            for task in &mut ctx_ref.tasks {
                if task.base.flags & HECTIC_KTHREAD != 0 {
                    rros_stop_kthread(&task.kthread);
                }
                task.rt_synch.destroy();
            }
        }
        rros_release_file(&mut ctx_ref.rfile).expect("release file failed");
        pr_debug!("hectic release!\n");
    }

    fn ioctl(
        ctx: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as core::ops::Deref>::Target,
        _file: &kernel::file::File,
        cmd: &mut file_operations::IoctlCommand,
    ) -> kernel::Result<i32> {
        pr_debug!("Hectic ioctl\n");
        let ctx = unsafe { &mut *ctx.get() };

        match cmd.cmd {
            RROS_HECIOC_SET_TASKS_COUNT => ctx.rtswitch_set_tasks_count(cmd.arg as u32),
            RROS_HECIOC_SET_CPU => {
                if cmd.arg as u32 > num_online_cpus() - 1 {
                    return Err(Error::EINVAL);
                }

                ctx.cpu = cmd.arg as u32;
                Ok(0)
            }
            RROS_HECIOC_SET_PAUSE => {
                ctx.pause_us = cmd.arg as u64;
                Ok(0)
            }
            RROS_HECIOC_REGISTER_UTASK => {
                let (uptrrd, uptrwt) = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticTaskIndex>(),
                    )
                    .reader_writer()
                };
                let mut task: HecticTaskIndex = uptrrd.read::<HecticTaskIndex>()?;

                match ctx.rtswitch_register_task(&mut task, 0) {
                    Ok(_) => uptrwt.write::<HecticTaskIndex>(&task).map(|_| 0),
                    Err(e) => Err(e),
                }
            }
            RROS_HECIOC_CREATE_KTASK => {
                let (uptrrd, uptrwt) = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticTaskIndex>(),
                    )
                    .reader_writer()
                };
                let mut task: HecticTaskIndex = uptrrd.read::<HecticTaskIndex>()?;

                match ctx.rtswitch_create_kthread(&mut task) {
                    Ok(_) => uptrwt.write::<HecticTaskIndex>(&task).map(|_| 0),
                    Err(e) => Err(e),
                }
            }
            RROS_HECIOC_PEND => {
                let uptrrd = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticTaskIndex>(),
                    )
                    .reader()
                };
                let task: HecticTaskIndex = uptrrd.read::<HecticTaskIndex>()?;
                ctx.rtswitch_pend_nrt(task.index)
            }
            RROS_HECIOC_SWITCH_TO => {
                let uptrrd = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticSwitchReq>(),
                    )
                    .reader()
                };
                let fromto: HecticSwitchReq = uptrrd.read::<HecticSwitchReq>()?;
                ctx.rtswitch_to_nrt(fromto.from, fromto.to)
            }
            RROS_HECIOC_GET_SWITCHES_COUNT => {
                let uptrwt = &mut unsafe {
                    UserSlicePtr::new(cmd.arg as *mut u8 as *mut c_types::c_void, size_of::<u32>())
                        .writer()
                };
                uptrwt.write::<u32>(&ctx.switches_count).map(|_| 0)
            }
            RROS_HECIOC_GET_LAST_ERROR => {
                // TODO: trace_fpu_breakage(ctx);
                let uptrwt = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticError>(),
                    )
                    .writer()
                };
                uptrwt.write::<HecticError>(&ctx.error).map(|_| 0)
            }
            RROS_HECIOC_LOCK_STAX => lock_stax(ctx, true),
            RROS_HECIOC_UNLOCK_STAX => unlock_stax(ctx, true),
            _ => Err(Error::ENOTTY),
        }
    }

    fn oob_ioctl(
        ctx: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as core::ops::Deref>::Target,
        _file: &kernel::file::File,
        cmd: &mut file_operations::IoctlCommand,
    ) -> kernel::Result<i32> {
        pr_debug!("Hectic oob_ioctl\n");
        let ctx = unsafe { &mut *ctx.get() };
        match cmd.cmd {
            RROS_HECIOC_PEND => {
                let uptrrd = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticTaskIndex>(),
                    )
                    .reader()
                };
                let task: HecticTaskIndex = uptrrd.read::<HecticTaskIndex>()?;
                ctx.rtswitch_pend_rt(task.index)
            }
            RROS_HECIOC_SWITCH_TO => {
                let uptrrd = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticSwitchReq>(),
                    )
                    .reader()
                };
                let fromto: HecticSwitchReq = uptrrd.read::<HecticSwitchReq>()?;
                ctx.rtswitch_to_rt(fromto.from, fromto.to)
            }
            RROS_HECIOC_GET_LAST_ERROR => {
                // TODO: trace_fpu_breakage(ctx);
                let uptrwt = &mut unsafe {
                    UserSlicePtr::new(
                        cmd.arg as *mut u8 as *mut c_types::c_void,
                        size_of::<HecticError>(),
                    )
                    .writer()
                };
                uptrwt.write::<HecticError>(&ctx.error).map(|_| 0)
            }
            RROS_HECIOC_LOCK_STAX => lock_stax(ctx, false),
            RROS_HECIOC_UNLOCK_STAX => unlock_stax(ctx, false),
            _ => Ok(0),
        }
    }
}

pub struct Hecticdev {
    pub dev: Pin<Box<chrdev::Registration<1>>>,
}

impl KernelModule for Hecticdev {
    fn init() -> Result<Self> {
        // SAFETY: `FP_FEATURES` is assigned only once during module initialization.
        unsafe {
            FP_FEATURES = rros_detect_fpu();
        }

        let hectic_class: Arc<class::Class> = Arc::try_new(class::Class::new(
            &THIS_MODULE,
            CStr::from_bytes_with_nul("hectic\0".as_bytes())?.as_char_ptr(),
        )?)?;

        let mut _dev = chrdev::Registration::new_pinned(c_str!("hecticdev"), 0, &THIS_MODULE)?;

        _dev.as_mut().register::<HecticFile>()?;

        let hectic_devt = _dev.as_mut().last_registered_devt().unwrap();
        // FIXME: temporarily used `device` abstraction to automatically create a device when the driver is loaded.
        unsafe {
            device::Device::raw_new(
                |d| {
                    d.devt = hectic_devt;
                    d.class = hectic_class.get_ptr();
                },
                CStr::from_bytes_with_nul("hectic\0".as_bytes())?,
            )
        };

        Ok(Hecticdev { dev: _dev })
    }
}

impl Drop for Hecticdev {
    fn drop(&mut self) {
        pr_debug!("Hectic exit\n");
    }
}
