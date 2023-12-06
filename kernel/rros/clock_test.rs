//clock.rs测试文件
//用于测试clock.rs里的函数正确&性
use crate::factory;
use crate::list::*;
use crate::{
    clock::*, factory::RrosElement, factory::RrosFactory, factory::RustFile, lock::*, sched::*,
    timer::*, RROS_OOB_CPUS,
};
use core::borrow::{Borrow, BorrowMut};
use core::cell::RefCell;
use core::cell::UnsafeCell;
use core::ops::Deref;
use core::ops::DerefMut;
use core::{mem::align_of, mem::size_of, todo};
use kernel::{
    bindings, c_types, cpumask::CpumaskT, double_linked_list::*, file_operations::FileOperations,
    ktime::*, percpu, percpu_defs, prelude::*, premmpt, spinlock_init, str::CStr, sync::Guard,
    sync::Lock, sync::SpinLock, sysfs, timekeeping,
};

//测试通过
pub fn test_do_clock_tick() -> Result<usize> {
    pr_info!("~~~test_do_clock_tick begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut a = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "zbw");

        let mut xx = Arc::try_new(a)?;
        xx.lock().add_status(RROS_TIMER_DEQUEUED);
        xx.lock().add_status(RROS_TIMER_PERIODIC);
        xx.lock().add_status(RROS_TIMER_RUNNING);
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_interval(1000);

        (*tmb).q.add_head(xx.clone());

        pr_info!("before do_clock_tick");
        do_clock_tick(&mut RROS_MONO_CLOCK, tmb);
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_do_clock_tick end~~~");
    Ok(0)
}

//测试通过
pub fn test_adjust_timer() -> Result<usize> {
    pr_info!("~~~test_adjust_timer begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut a = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut xx = Arc::try_new(a)?;
        xx.lock().add_status(RROS_TIMER_DEQUEUED);
        xx.lock().add_status(RROS_TIMER_PERIODIC);
        xx.lock().add_status(RROS_TIMER_RUNNING);
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_interval(1000);

        // (*tmb).q.add_head(xx.clone());

        pr_info!("before adjust_timer");
        adjust_timer(&RROS_MONO_CLOCK, xx.clone(), &mut (*tmb).q, 100);
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_adjust_timer end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_adjust_timers() -> Result<usize> {
    pr_info!("~~~test_rros_adjust_timers begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut a = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut b = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut b) };
        spinlock_init!(pinned, "b");

        let mut xx = Arc::try_new(a)?;
        let mut yy = Arc::try_new(b)?;

        let add1 = &mut xx.lock().start_date as *mut KtimeT;
        pr_info!("add1 is {:p}", add1);

        let interval_add = &mut xx.lock().interval as *mut KtimeT;
        pr_info!("add interval is {:p}", interval_add);

        let add2 = &mut xx.lock().start_date as *mut KtimeT;
        pr_info!("add2 is {:p}", add2);

        // xx.lock().add_status(RROS_TIMER_FIRED);
        xx.lock().add_status(RROS_TIMER_PERIODIC);
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_interval(1000);

        yy.lock().add_status(RROS_TIMER_PERIODIC);
        yy.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        yy.lock().set_interval(1000);

        (*tmb).q.add_head(xx.clone());
        (*tmb).q.add_head(yy.clone());

        pr_info!("before adjust_timer");
        rros_adjust_timers(&mut RROS_MONO_CLOCK, 100);
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_rros_adjust_timers end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_stop_timers() -> Result<usize> {
    pr_info!("~~~test_rros_stop_timers begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut a = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut b = SpinLock::new(RrosTimer::new(580000000));
        let pinned = unsafe { Pin::new_unchecked(&mut b) };
        spinlock_init!(pinned, "b");

        let mut xx = Arc::try_new(a)?;
        let mut yy = Arc::try_new(b)?;

        xx.lock().add_status(RROS_TIMER_PERIODIC);
        xx.lock().add_status(RROS_TIMER_DEQUEUED);
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_interval(1000);
        xx.lock().set_base(tmb);

        yy.lock().add_status(RROS_TIMER_PERIODIC);
        yy.lock().add_status(RROS_TIMER_DEQUEUED);
        yy.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        yy.lock().set_interval(1000);
        yy.lock().set_base(tmb);

        (*tmb).q.add_head(xx.clone());
        (*tmb).q.add_head(yy.clone());

        pr_info!("before rros_adjust_timers");
        rros_stop_timers(&RROS_MONO_CLOCK);
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_rros_stop_timers end~~~");
    Ok(0)
}
