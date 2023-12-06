#![no_std]
#![feature(allocator_api)]
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

pub fn test_enqueue_by_index() -> Result<usize> {
    pr_info!("~~~test_double_linked_list begin~~~");
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
        let mut a = SpinLock::new(RrosTimer::new(4));
        let pinned = unsafe { Pin::new_unchecked(&mut a) };
        spinlock_init!(pinned, "a");

        let mut xx = Arc::try_new(x)?;
        let mut yy = Arc::try_new(y)?;
        let mut zz = Arc::try_new(z)?;
        let mut aa = Arc::try_new(a)?;

        (*tmb).q.add_head(xx.clone());
        (*tmb).q.add_head(yy.clone());
        (*tmb).q.add_head(zz.clone());

        pr_info!("before enqueue_by_index");
        (*tmb).q.enqueue_by_index(2, aa);
        pr_info!("len is {}", (*tmb).q.len());

        for i in 1..=(*tmb).q.len() {
            let mut _x = (*tmb).q.get_by_index(i).unwrap().value.clone();
            pr_info!("data of x is {}", _x.lock().get_date());
        }
    }
    pr_info!("~~~test_double_linked_list end~~~");
    Ok(0)
}
