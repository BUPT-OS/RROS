use kernel::{c_types, cpumask, prelude::*, spinlock_init, str::CStr, sync::SpinLock};

pub fn raw_spin_lock_init(lock: &mut SpinLock<i32>) {
    *lock = unsafe { SpinLock::new(1) };
    let pinned = unsafe { Pin::new_unchecked(lock) };
    spinlock_init!(pinned, "timerbase");
}

extern "C" {
    fn rust_helper_hard_local_irq_save() -> c_types::c_ulong;
    fn rust_helper_hard_local_irq_restore(flags: c_types::c_ulong);
    fn rust_helper_preempt_enable();
    fn rust_helper_preempt_disable();
    // fn rust_helper_raw_spin_lock_irqsave();
    // fn rust_helper_raw_spin_unlock_irqrestore();
}

// TODO: modify this when we have the real smp support
pub fn raw_spin_lock_irqsave() -> c_types::c_ulong {
    let flags = unsafe { rust_helper_hard_local_irq_save() };
    // unsafe{rust_helper_preempt_disable();}
    return flags;
}

// TODO: modify this when we have the real smp support
pub fn raw_spin_unlock_irqrestore(flags: c_types::c_ulong) {
    unsafe {
        rust_helper_hard_local_irq_restore(flags);
        // rust_helper_preempt_enable();
    }
}

// pub fn right_raw_spin_lock_irqsave(lock: *mut spinlock_t, flags: *mut u32) {
//     // let flags = unsafe { rust_helper_raw_local_irq_save() };
//     unsafe { rust_helper_raw_local_irq_save() };
//     // unsafe{rust_helper_preempt_disable();}
//     // return flags;
// }

// pub fn right_raw_spin_unlock_irqrestore(lock: *mut spinlock_t, flags: *mut u32) {
//     // let flags = unsafe { rust_helper_raw_local_irq_save() };
//     unsafe { rust_helper_raw_spin_unlock_irqrestore() };
//     // unsafe{rust_helper_preempt_disable();}
//     // return flags;
// }