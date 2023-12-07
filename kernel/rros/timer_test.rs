//timer.rs测试文件
//用于测试timer.rs里的函数正确性
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
pub fn test_rros_insert_tnode() -> Result<usize> {
    pr_info!("~~~test_rros_insert_tnode begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(12));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut y = SpinLock::new(RrosTimer::new(2));
        let pinned = unsafe { Pin::new_unchecked(&mut y) };
        spinlock_init!(pinned, "y");
        let mut z = SpinLock::new(RrosTimer::new(31));
        let pinned = unsafe { Pin::new_unchecked(&mut z) };
        spinlock_init!(pinned, "z");
        let mut a = SpinLock::new(RrosTimer::new(14));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut xx = Arc::try_new(x)?;
        let mut yy = Arc::try_new(y)?;
        let mut zz = Arc::try_new(z)?;
        let mut aa = Arc::try_new(a)?;

        pr_info!("before enqueue_by_index");
        rros_insert_tnode(&mut (*tmb).q, xx);
        rros_insert_tnode(&mut (*tmb).q, yy);
        rros_insert_tnode(&mut (*tmb).q, zz);
        rros_insert_tnode(&mut (*tmb).q, aa);

        pr_info!("len is {}", (*tmb).q.len());

        for i in 1..=(*tmb).q.len() {
            let mut _x = (*tmb).q.get_by_index(i).unwrap().value.clone();
            pr_info!("data of x is {}", _x.lock().get_date());
        }
    }
    pr_info!("~~~test_rros_insert_tnode end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_enqueue_timer() -> Result<usize> {
    pr_info!("~~~test_rros_insert_tnode begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(12));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut y = SpinLock::new(RrosTimer::new(2));
        let pinned = unsafe { Pin::new_unchecked(&mut y) };
        spinlock_init!(pinned, "y");
        let mut z = SpinLock::new(RrosTimer::new(31));
        let pinned = unsafe { Pin::new_unchecked(&mut z) };
        spinlock_init!(pinned, "z");
        let mut a = SpinLock::new(RrosTimer::new(14));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut xx = Arc::try_new(x)?;
        let mut yy = Arc::try_new(y)?;
        let mut zz = Arc::try_new(z)?;
        let mut aa = Arc::try_new(a)?;

        pr_info!("before enqueue_by_index");
        rros_enqueue_timer(xx, &mut (*tmb).q);
        rros_enqueue_timer(yy, &mut (*tmb).q);
        rros_enqueue_timer(zz, &mut (*tmb).q);
        rros_enqueue_timer(aa, &mut (*tmb).q);

        pr_info!("len is {}", (*tmb).q.len());

        for i in 1..=(*tmb).q.len() {
            let mut _x = (*tmb).q.get_by_index(i).unwrap().value.clone();
            pr_info!("data of x is {}", _x.lock().get_date());
        }
        pr_info!("qufan RROS_TIMER_DEQUEUED is {}", !RROS_TIMER_DEQUEUED);
    }
    pr_info!("~~~test_rros_insert_tnode end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_get_timer_gravity() -> Result<usize> {
    pr_info!("~~~test_rros_get_timer_gravity begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_status(RROS_TIMER_KGRAVITY);
        pr_info!("kernel gravity is {}", rros_get_timer_gravity(xx.clone()));

        xx.lock().set_status(RROS_TIMER_UGRAVITY);
        pr_info!("user gravity is {}", rros_get_timer_gravity(xx.clone()));

        xx.lock().set_status(0);
        pr_info!("irq gravity is {}", rros_get_timer_gravity(xx.clone()));
    }
    pr_info!("~~~test_rros_get_timer_gravity end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_update_timer_date() -> Result<usize> {
    pr_info!("~~~test_rros_update_timer_date begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_start_date(2);
        xx.lock().set_periodic_ticks(3);
        xx.lock().set_interval(8);
        xx.lock().set_status(RROS_TIMER_UGRAVITY);

        rros_update_timer_date(xx.clone());
        pr_info!("xx date is {}", xx.lock().get_date());
    }
    pr_info!("~~~test_rros_update_timer_date end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_get_timer_next_date() -> Result<usize> {
    pr_info!("~~~test_rros_get_timer_next_date begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;

        xx.lock().set_start_date(2);
        xx.lock().set_periodic_ticks(3);
        xx.lock().set_interval(8);

        pr_info!("xx next date is {}", rros_get_timer_next_date(xx.clone()));
    }
    pr_info!("~~~test_rros_get_timer_next_date end~~~");
    Ok(0)
}

//测试通过
pub fn test_timer_at_front() -> Result<usize> {
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut y = SpinLock::new(RrosTimer::new(2));
        let pinned = unsafe { Pin::new_unchecked(&mut y) };
        spinlock_init!(pinned, "y");
        let mut z = SpinLock::new(RrosTimer::new(3));
        let pinned = unsafe { Pin::new_unchecked(&mut z) };
        spinlock_init!(pinned, "z");

        let mut xx = Arc::try_new(x)?;
        let mut yy = Arc::try_new(y)?;
        let mut zz = Arc::try_new(z)?;

        xx.lock().set_base(tmb);
        yy.lock().set_base(tmb);
        zz.lock().set_base(tmb);
        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_rq(rq);
        yy.lock().set_rq(rq);
        zz.lock().set_rq(rq);
        (*tmb).q.add_head(xx.clone());
        (*tmb).q.add_head(yy.clone());
        (*tmb).q.add_head(zz.clone());

        //测试第一个if分支
        if timer_at_front(zz.clone()) == true {
            pr_info!("test_timer_at_front if1 true");
        } else {
            pr_info!("test_timer_at_front if1 false");
        }

        //测试第二个if分支
        if timer_at_front(yy.clone()) == true {
            pr_info!("test_timer_at_front if2 true");
        } else {
            pr_info!("test_timer_at_front if2 false");
        }
    }
    Ok(0)
}

//测试通过
pub fn test_rros_timer_deactivate() -> Result<usize> {
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut y = SpinLock::new(RrosTimer::new(2));
        let pinned = unsafe { Pin::new_unchecked(&mut y) };
        spinlock_init!(pinned, "y");
        let mut z = SpinLock::new(RrosTimer::new(3));
        let pinned = unsafe { Pin::new_unchecked(&mut z) };
        spinlock_init!(pinned, "z");

        let mut xx = Arc::try_new(x)?;
        let mut yy = Arc::try_new(y)?;
        let mut zz = Arc::try_new(z)?;

        xx.lock().set_base(tmb);
        yy.lock().set_base(tmb);
        zz.lock().set_base(tmb);

        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_rq(rq);
        yy.lock().set_rq(rq);
        zz.lock().set_rq(rq);
        (*tmb).q.add_head(xx.clone());
        (*tmb).q.add_head(yy.clone());
        (*tmb).q.add_head(zz.clone());

        zz.lock().set_status(RROS_TIMER_DEQUEUED);

        if rros_timer_deactivate(zz.clone()) {
            pr_info!("test_rros_timer_deactivate: success");
        } else {
            pr_info!("test_rros_timer_deactivate: failed");
        }

        pr_info!(
            "test_rros_timer_deactivate: len of tmb is {}",
            (*tmb).q.len()
        );
    }
    Ok(0)
}

//测试通过
pub fn test_rros_get_timer_expiry() -> Result<usize> {
    pr_info!("~~~test_rros_get_timer_expiry begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_date(11);
        xx.lock().set_status(0);

        pr_info!("xx next date is {}", rros_get_timer_expiry(xx.clone()));
    }
    pr_info!("~~~test_rros_get_timer_expiry end~~~");
    Ok(0)
}

//测试通过
pub fn test___rros_get_timer_delta() -> Result<usize> {
    pr_info!("~~~test___rros_get_timer_delta begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_date(1111111111111);
        xx.lock().set_status(0);

        pr_info!("xx delta is {}", __rros_get_timer_delta(xx.clone()));

        xx.lock().set_date(0);
        xx.lock().set_status(RROS_TIMER_UGRAVITY);

        pr_info!("xx delta is {}", __rros_get_timer_delta(xx.clone()));
    }
    pr_info!("~~~test___rros_get_timer_delta end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_get_timer_delta() -> Result<usize> {
    pr_info!("~~~test_rros_get_timer_delta begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_date(1111111111111);
        xx.lock().set_status(RROS_TIMER_RUNNING);

        pr_info!("xx delta is {}", rros_get_timer_delta(xx.clone()));

        xx.lock().set_date(0);
        xx.lock().set_status(RROS_TIMER_PERIODIC);

        pr_info!("xx delta is {}", rros_get_timer_delta(xx.clone()));
    }
    pr_info!("~~~test_rros_get_timer_delta end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_get_timer_date() -> Result<usize> {
    pr_info!("~~~test_rros_get_timer_date begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");
        let mut xx = Arc::try_new(x)?;
        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);

        xx.lock().set_date(11);
        xx.lock().set_status(RROS_TIMER_RUNNING);

        pr_info!("xx next date is {}", rros_get_timer_date(xx.clone()));

        xx.lock().set_status(RROS_TIMER_PERIODIC);
        pr_info!("xx next date is {}", rros_get_timer_date(xx.clone()));
    }
    pr_info!("~~~test_rros_get_timer_date end~~~");
    Ok(0)
}

//测试通过
pub fn test_program_timer() -> Result<usize> {
    pr_info!("~~~test_program_timer begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");

        let mut xx = Arc::try_new(x)?;

        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_rq(rq);
        xx.lock().set_base(tmb);
        let tmb1 = xx.lock().get_base();

        program_timer(xx.clone(), &mut (*tmb1).q);

        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_program_timer end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_start_timer() -> Result<usize> {
    pr_info!("~~~test_rros_start_timer begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(17));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");

        let mut xx = Arc::try_new(x)?;

        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_rq(rq);
        xx.lock().set_base(tmb);
        pr_info!("before program_timer");
        rros_start_timer(xx.clone(), 333, 222);

        pr_info!("timer date is {}", xx.lock().get_date());
        pr_info!("timer start date is {}", xx.lock().get_start_date());
        pr_info!("timer interval is {}", xx.lock().get_interval());
        pr_info!("timer status is {}", xx.lock().get_status());

        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_rros_start_timer end~~~");
    Ok(0)
}

//测试通过
pub fn test_stop_timer_locked() -> Result<usize> {
    pr_info!("~~~test_stop_timer_locked begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(17));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");

        let mut xx = Arc::try_new(x)?;

        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_rq(rq);
        xx.lock().set_base(tmb);
        xx.lock().set_status(RROS_TIMER_RUNNING);
        pr_info!("before stop_timer_locked");
        stop_timer_locked(xx.clone());
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_stop_timer_locked end~~~");
    Ok(0)
}

//测试通过
pub fn test_rros_destroy_timer() -> Result<usize> {
    pr_info!("~~~test_rros_destroy_timer begin~~~");
    unsafe {
        let mut tmb = rros_percpu_timers(&RROS_MONO_CLOCK, 0);
        let mut x = SpinLock::new(RrosTimer::new(17));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");

        let mut xx = Arc::try_new(x)?;

        let mut _rq = rros_rq::new()?;
        let mut rq = &mut _rq as *mut rros_rq;

        xx.lock().set_clock(&mut RROS_MONO_CLOCK as *mut RrosClock);
        xx.lock().set_rq(rq);
        xx.lock().set_base(tmb);
        xx.lock().set_status(RROS_TIMER_RUNNING);
        pr_info!("before rros_destroy_timer");
        rros_destroy_timer(xx.clone());
        let xx_lock_rq = xx.lock().get_rq();
        let xx_lock_base = xx.lock().get_base();
        if xx_lock_rq == 0 as *mut RrosRq {
            pr_info!("xx rq is none");
        }
        if xx_lock_base == 0 as *mut RrosTimerbase {
            pr_info!("xx base is none");
        }
        pr_info!("len of tmb is {}", (*tmb).q.len());
    }
    pr_info!("~~~test_rros_destroy_timer end~~~");
    Ok(0)
}

pub fn handler(timer: &RrosTimer) {
    pr_info!("success");
}

pub fn test_get_handler() -> Result<usize> {
    pr_info!("~~~test_get_handler begin~~~");
    unsafe {
        let mut x = SpinLock::new(RrosTimer::new(17));
        let pinned = unsafe { Pin::new_unchecked(&mut x) };
        spinlock_init!(pinned, "x");

        let mut xx = Arc::try_new(x)?;

        //xx.lock().set_handler(Some(handler));
        //let handler = xx.lock().get_handler();
        //handler(xx.lock().deref());
    }
    pr_info!("~~~test_get_handler end~~~");
    Ok(0)
}