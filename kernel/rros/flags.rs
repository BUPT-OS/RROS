use crate::{
    clock::{RrosClock, RROS_MONO_CLOCK},
    sched::rros_schedule,
    timeout::{RrosTmode, RROS_INFINITE},
    wait::{RrosWaitQueue, RROS_WAIT_PRIO},
};
use core::{cell::Cell, ptr::NonNull};
use kernel::bindings;

pub struct RrosFlag {
    pub wait: RrosWaitQueue,
    pub raised: Cell<bool>,
}
impl RrosFlag {
    pub fn new() -> Self {
        RrosFlag {
            wait: unsafe {
                RrosWaitQueue::new(
                    &mut RROS_MONO_CLOCK as *mut RrosClock,
                    RROS_WAIT_PRIO as i32,
                )
            },
            raised: Cell::new(false),
        }
    }

    #[inline]
    pub fn init(&mut self) {
        self.wait.init(
            unsafe { &mut RROS_MONO_CLOCK as *mut RrosClock },
            RROS_WAIT_PRIO as i32,
        );
        self.raised = Cell::new(false);
    }

    #[allow(dead_code)]
    #[inline]
    pub fn destory(&mut self) {
        self.wait.destory();
    }

    // #[inline]
    // pub fn wait_timeout(&mut self, timeout : bindings:KtimeT,) -> bool{
    //     if self.raised == false{
    //         self.wait.wait_timeout(timeout);
    //     }
    //     self.raised
    // }
    #[inline]
    pub fn read(&self) -> bool {
        if self.raised.get() {
            self.raised.set(false);
            return true;
        }
        false
    }
    #[inline]
    pub fn wait(&mut self) -> i32 {
        // TODO:尝试绕开不可变借用的限制
        let mut x = unsafe { NonNull::new_unchecked(&self.wait as *const _ as *mut RrosWaitQueue) };
        unsafe {
            x.as_mut()
                .wait_timeout(RROS_INFINITE, RrosTmode::RrosRel, || self.read())
        }
    }

    #[inline]
    pub fn raise(&mut self) {
        // let flags = unsafe{bindings::_raw_spin_lock_irqsave(&mut self.wait.lock as *const _ as *mut bindings::raw_spinlock)};
        let flags = unsafe {
            rust_helper_raw_spin_lock_irqsave(
                &mut self.wait.lock as *const _ as *mut bindings::hard_spinlock_t,
            )
        };
        self.raised.set(true);
        self.wait.flush_locked(0);
        unsafe {
            rust_helper_raw_spin_unlock_irqrestore(
                &mut self.wait.lock as *const _ as *mut bindings::hard_spinlock_t,
                flags,
            )
        };
        // unsafe{bindings::_raw_spin_unlock_irqrestore(&mut self.wait.lock as *const _ as *mut bindings::raw_spinlock, flags)};

        unsafe { rros_schedule() };
    }
}

// pub fn test_flag(){
//     use crate::thread::KthreadRunner;
//     let mut runner = KthreadRunner::new_empty();
//     let mut global_flag : Arc<RrosFlag>    = unsafe{Arc::try_new(core::mem::zeroed()).unwrap()};
//     let x = Arc::get_mut(&mut flag).unwrap();
//     x.init();
//     drop(x);
//     let mut runner = KthreadRunner::new_empty();
//     runner.init(Box::try_new(move||{
//         let mut flag = global_flag.clone();
//         for i in 0..10{
//             flag.as_ref().wait()
//         }
//     }).unwrap());

// }

extern "C" {
    fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
    fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
}
