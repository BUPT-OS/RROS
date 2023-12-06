use crate::{clock, sched, thread, tick, timer};
use alloc::rc::Rc;
use core::cell::RefCell;
use kernel::{
    bindings, c_str, c_types,
    prelude::*,
    spinlock_init,
    sync::{Guard, Lock, SpinLock},
};

pub fn test_this_rros_rq_thread() -> Result<usize> {
    pr_info!("~~~test_this_rros_rq_thread begin~~~");
    let curr = sched::this_rros_rq_thread();
    match curr {
        None => {
            pr_info!("curr is None");
        }
        Some(x) => {
            pr_info!("curr is not None ");
        }
    };
    pr_info!("~~~test_this_rros_rq_thread end~~~");
    Ok(0)
}

pub fn test_cpu_smp() -> Result<usize> {
    pr_info!("~~~test_cpu_smp begin~~~");
    let rq = sched::this_rros_rq();
    unsafe {
        pr_info!("cpu is {}", (*rq).cpu);
    }
    pr_info!("~~~test_cpu_smp end~~~");
    Ok(0)
}

pub fn test_rros_set_resched() -> Result<usize> {
    pr_info!("~~~test_rros_set_resched begin~~~");
    let rq = sched::this_rros_rq();
    unsafe {
        pr_info!("before this_rros_rq flags is {}", (*rq).flags);
    }
    sched::rros_set_resched(Some(rq));
    unsafe {
        pr_info!("after this_rros_rq flags is {}", (*rq).flags);
    }
    pr_info!("~~~test_rros_set_resched end~~~");
    Ok(0)
}

pub fn test_rros_in_irq() -> Result<usize> {
    pr_info!("~~~test_rros_set_resched begin~~~");
    let rq = sched::this_rros_rq();
    unsafe {
        pr_info!("before this_rros_rq flags is {}", (*rq).flags);
    }
    sched::rros_set_resched(Some(rq));
    unsafe {
        pr_info!("after this_rros_rq flags is {}", (*rq).flags);
    }
    pr_info!("~~~test_rros_set_resched end~~~");
    Ok(0)
}
