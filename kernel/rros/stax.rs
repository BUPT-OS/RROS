use core::{
    marker::PhantomPinned,
    sync::atomic::{AtomicU32, Ordering},
};

use crate::{
    clock::{RrosClock, RROS_MONO_CLOCK},
    sched::{rros_schedule, RrosThread, RrosValue},
    thread::{rros_current, rros_kick_thread, rros_notify_thread, RROS_HMDIAG_STAGEX, T_WOSX},
    timeout::{RrosTmode::RrosRel, RROS_INFINITE},
    wait::{RrosWaitQueue, RROS_WAIT_PRIO},
};

use kernel::{
    bindings,
    c_types::c_void,
    error::Result,
    irq_work::IrqWork,
    prelude::*,
    premmpt::running_inband,
    sched::{schedule, set_current_state},
    sync::{Lock, SpinLock},
    task::Task,
    waitqueue::{WaitQueueEntry, WaitQueueHead},
};

/// Gate marker for in-band activity.
pub const STAX_INBAND_BIT: u32 = 1 << 31;
/// The stax is being claimed by the converse stage.
pub const STAX_CLAIMED_BIT: u32 = 1 << 30;
/// The number of threads currently traversing the section.
pub const STAX_CONCURRENCY_MASK: u32 = !(STAX_INBAND_BIT | STAX_CLAIMED_BIT);

/// A lock used to block access to a resource by both in-band and out-of-band threads
/// A thread of one stage can acquire a lock
/// only after all threads of the converse stage have released the lock
pub struct RrosStax {
    gate: AtomicU32,
    oob_wait: RrosWaitQueue,
    inband_wait: WaitQueueHead,
    irq_work: IrqWork,
    _marker: PhantomPinned,
}

impl RrosStax {
    /// Create a new RrosStax.
    #[allow(dead_code)]
    pub fn new() -> Self {
        let stax: RrosStax = RrosStax {
            gate: AtomicU32::new(0),
            oob_wait: unsafe {
                RrosWaitQueue::new(
                    &mut RROS_MONO_CLOCK as *mut RrosClock,
                    RROS_WAIT_PRIO as i32,
                )
            },
            inband_wait: WaitQueueHead::default(),
            irq_work: IrqWork::new(),
            _marker: PhantomPinned,
        };
        stax
    }
    /// Initialize the RrosStax.
    #[allow(dead_code)]
    pub fn init(&mut self) -> Result<usize> {
        self.oob_wait.init(
            unsafe { &mut RROS_MONO_CLOCK as *mut RrosClock },
            RROS_WAIT_PRIO as i32,
        );
        self.inband_wait.init();
        self.irq_work.init_irq_work(c_wakeup_inband_waiters)
    }
    /// Destory the RrosStax.
    #[allow(dead_code)]
    pub fn destory(&mut self) {
        self.oob_wait.destory();
    }
    /// Lock the RrosStax.
    /// When a lock is held by a thread in the converse stage, the current thread is blocked.
    #[allow(dead_code)]
    pub fn lock(&mut self) -> Result<()> {
        //TODO: EVL_WARN_ON(CORE,evl_in_irq());
        if running_inband().is_ok() {
            self.lock_from_inband(true)
        } else {
            self.lock_from_oob(true)
        }
    }
    /// Try to lock the RrosStax.
    /// When the lock is held by a thread in the converse stage, a value is returned immediately.
    #[allow(dead_code)]
    pub fn try_lock(&mut self) -> Result<()> {
        if running_inband().is_ok() {
            self.lock_from_inband(false)
        } else {
            self.lock_from_oob(false)
        }
    }
    /// Unlock the RrosStax.
    /// When the last thread of this stage releases the lock,
    /// if there are threads of the converse stage waiting for the lock,
    /// wakes up the threads.
    #[allow(dead_code)]
    pub fn unlock(&mut self) {
        //TODO: EVL_WARN_ON(CORE, evl_in_irq());
        if running_inband().is_ok() {
            self.unlock_from_inband();
        } else {
            self.unlock_from_oob();
        }
    }
    fn lock_from_inband(&mut self, wait: bool) -> Result<()> {
        let mut old: u32;
        let mut new: u32;

        loop {
            old = self.gate.load(Ordering::Acquire);
            if (old & STAX_CONCURRENCY_MASK) != 0 {
                old |= STAX_INBAND_BIT;
            }
            new = ((old & !STAX_INBAND_BIT) + 1) | STAX_INBAND_BIT;

            match self
                .gate
                .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => break,
                Err(prev) => {
                    if inband_may_access(prev) {
                        continue;
                    }
                    if !wait {
                        return Err(Error::EAGAIN);
                    }
                    self.claim_stax_from_inband(prev)?
                }
            }
        }

        Ok(())
    }
    fn claim_stax_from_inband(&mut self, gateval: u32) -> Result<()> {
        let mut ib_flags: u64;
        let mut oob_flags: u64;
        let mut wq_entry = WaitQueueEntry::default();
        let mut old: u32;
        let mut new: u32;
        let mut ret = Ok(());

        wq_entry.init_wait_entry(0);
        ib_flags = self.inband_wait.spin_lock_irqsave();
        oob_flags = self.oob_wait.lock.raw_spin_lock_irqsave();

        'outer: loop {
            'inter: loop {
                if (gateval & STAX_CLAIMED_BIT) != 0 {
                    old = self.gate.load(Ordering::Acquire);
                    if (old & STAX_INBAND_BIT) != 0 {
                        break 'outer;
                    }
                    if (old & STAX_CLAIMED_BIT) != 0 {
                        break 'inter;
                    }
                } else {
                    old = gateval;
                }

                loop {
                    new = old | STAX_CLAIMED_BIT;

                    match self
                        .gate
                        .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
                    {
                        Ok(_) => break 'inter,
                        Err(prev) => {
                            old = prev;
                            if (prev & STAX_INBAND_BIT) != 0 {
                                break 'outer;
                            }
                            if (prev & STAX_CLAIMED_BIT) != 0 {
                                break 'inter;
                            }
                        }
                    }
                }
            }

            loop {
                if wq_entry.list_empty() {
                    self.inband_wait.add_wait_queue(&mut wq_entry);
                }
                if inband_may_access(self.gate.load(Ordering::Acquire)) {
                    break;
                }
                //TODO: wrap the constant from bindings
                set_current_state(bindings::TASK_UNINTERRUPTIBLE as i64);
                self.oob_wait.lock.raw_spin_unlock_irqrestore(oob_flags);
                self.inband_wait.spin_unlock_irqrestore(ib_flags);
                schedule();
                ib_flags = self.inband_wait.spin_lock_irqsave();
                oob_flags = self.oob_wait.lock.raw_spin_lock_irqsave();

                if Task::current().signal_pending() {
                    ret = Err(Error::ERESTARTSYS);
                    break;
                }
            }

            wq_entry.list_del();

            if !self.inband_wait.waitqueue_active() {
                old = self.gate.load(Ordering::Acquire);
                loop {
                    if let Err(prev) = self.gate.compare_exchange(
                        old,
                        old & !STAX_CLAIMED_BIT,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    ) {
                        old = prev;
                    } else {
                        break;
                    }
                }
            }
            break;
        }

        self.oob_wait.lock.raw_spin_unlock_irqrestore(oob_flags);
        self.inband_wait.spin_unlock_irqrestore(ib_flags);

        ret
    }
    fn lock_from_oob(&mut self, wait: bool) -> Result<()> {
        let mut old: u32;

        loop {
            old = self.gate.load(Ordering::Acquire) & !STAX_INBAND_BIT;

            match self
                .gate
                .compare_exchange(old, old + 1, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => break,
                Err(prev) => {
                    if oob_may_access(prev) {
                        continue;
                    }
                    if !wait {
                        return Err(Error::EAGAIN);
                    }
                    self.claim_stax_from_oob(prev)?
                }
            }
        }

        Ok(())
    }
    fn claim_stax_from_oob(&mut self, gateval: u32) -> Result<()> {
        let ptr = rros_current();
        let curr: Arc<SpinLock<RrosThread>>;
        unsafe {
            curr = Arc::from_raw(ptr);
            Arc::increment_strong_count(ptr);
        }
        let mut old: u32;
        let mut new: u32;
        let mut ret = Ok(());
        let mut flags: u64;
        let mut notify: bool = false;

        flags = self.oob_wait.lock.raw_spin_lock_irqsave();

        'outer: loop {
            'inter: loop {
                if (gateval & STAX_CLAIMED_BIT) != 0 {
                    old = self.gate.load(Ordering::Acquire);
                    if (old & STAX_INBAND_BIT) == 0 {
                        break 'outer;
                    }
                    if (old & STAX_CLAIMED_BIT) != 0 {
                        break 'inter;
                    }
                } else {
                    old = gateval;
                }

                loop {
                    new = old | STAX_CLAIMED_BIT;

                    match self
                        .gate
                        .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
                    {
                        Ok(_) => break 'inter,
                        Err(prev) => {
                            old = prev;
                            if (prev & STAX_INBAND_BIT) == 0 {
                                break 'outer;
                            }
                            if (prev & STAX_CLAIMED_BIT) != 0 {
                                break 'inter;
                            }
                        }
                    }
                }
            }

            if (curr.lock().state & T_WOSX) != 0 {
                notify = true;
            }

            loop {
                if oob_may_access(self.gate.load(Ordering::Acquire)) {
                    break;
                }
                self.oob_wait.locked_add(RROS_INFINITE, RrosRel);
                self.oob_wait.lock.raw_spin_unlock_irqrestore(flags);
                //TODO: wait this method return Result
                let res = self.oob_wait.wait_schedule();
                flags = self.oob_wait.lock.raw_spin_lock_irqsave();
                if res != 0 {
                    ret = Err(Error::from_kernel_errno(res));
                    break;
                }
            }

            if !self.oob_wait.is_active() {
                old = self.gate.load(Ordering::Acquire);
                loop {
                    match self.gate.compare_exchange(
                        old,
                        old & !STAX_CLAIMED_BIT,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    ) {
                        Ok(_) => break,
                        Err(prev) => {
                            old = prev;
                        }
                    }
                }
            }

            break;
        }

        self.oob_wait.lock.raw_spin_unlock_irqrestore(flags);

        if notify {
            let cur_thread = unsafe { &mut *(*rros_current()).locked_data().get() };
            rros_notify_thread(
                cur_thread as *mut RrosThread,
                RROS_HMDIAG_STAGEX,
                RrosValue::new(),
            )?;
            rros_kick_thread(curr, 0);
        }
        ret
    }
    fn unlock_from_inband(&mut self) {
        let mut old: u32;
        let mut new: u32;
        let flags: u64;

        old = self.gate.load(Ordering::Acquire);

        while (old & STAX_CLAIMED_BIT) == 0 {
            if !inband_unlock_sane(old) {
                return;
            }
            old &= !STAX_CLAIMED_BIT;
            new = (old & !STAX_INBAND_BIT) - 1;
            if (new & STAX_CONCURRENCY_MASK) != 0 {
                new |= STAX_INBAND_BIT;
            }

            match self
                .gate
                .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => return,
                Err(prev) => {
                    old = prev;
                }
            }
        }

        flags = self.oob_wait.lock.raw_spin_lock_irqsave();

        'out: loop {
            loop {
                if !inband_unlock_sane(old) {
                    break 'out;
                }
                new = (old & !STAX_INBAND_BIT) - 1;
                if (new & STAX_CONCURRENCY_MASK) != 0 {
                    new |= STAX_INBAND_BIT;
                }
                match self
                    .gate
                    .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
                {
                    Ok(_) => break,
                    Err(prev) => {
                        old = prev;
                    }
                }
            }

            if (new & STAX_CONCURRENCY_MASK) == 0 {
                self.oob_wait.flush_locked(0);
            }

            break;
        }

        self.oob_wait.lock.raw_spin_unlock_irqrestore(flags);
        unsafe { rros_schedule() }
    }
    fn unlock_from_oob(&mut self) {
        let mut old: u32;
        let mut new: u32;
        let flags: u64;

        old = self.gate.load(Ordering::Acquire);

        while (old & STAX_CLAIMED_BIT) == 0 {
            if !oob_unlock_sane(old) {
                return;
            }
            old &= !STAX_CLAIMED_BIT;
            new = old - 1;
            match self
                .gate
                .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => return,
                Err(prev) => {
                    old = prev;
                }
            }
        }

        flags = self.oob_wait.lock.raw_spin_lock_irqsave();

        loop {
            new = old - 1;
            if !oob_unlock_sane(old) {
                break;
            }
            match self
                .gate
                .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => break,
                Err(prev) => {
                    old = prev;
                }
            }
        }

        self.oob_wait.lock.raw_spin_unlock_irqrestore(flags);

        if (new & STAX_CONCURRENCY_MASK) == 0 {
            match self.irq_work.irq_work_queue() {
                Ok(_) => {}
                Err(_) => {
                    pr_err!("irq_work_queue failed")
                }
            }
        }
    }
}

fn inband_unlock_sane(_gateval: u32) -> bool {
    /*!EVL_WARN_ON(CORE,    EVL_WARN_ON(CORE,?) seem to be always true
    !(gateval & STAX_CONCURRENCY_MASK) ||
    !(gateval & STAX_INBAND_BIT))*/
    // (gateval & STAX_CONCURRENCY_MASK) == 0 || (gateval & STAX_INBAND_BIT) == 0
    true
}
fn oob_unlock_sane(_gateval: u32) -> bool {
    /*!EVL_WARN_ON(CORE,
    !(gateval & STAX_CONCURRENCY_MASK) ||
    gateval & STAX_INBAND_BIT); */
    // (gateval & STAX_CONCURRENCY_MASK) == 0 || (gateval & STAX_INBAND_BIT) != 0
    true
}
fn inband_may_access(gateval: u32) -> bool {
    (gateval & STAX_CONCURRENCY_MASK) == 0 || (gateval & STAX_INBAND_BIT) != 0
}
fn oob_may_access(gateval: u32) -> bool {
    (gateval & STAX_INBAND_BIT) == 0
}
#[no_mangle]
unsafe extern "C" fn c_wakeup_inband_waiters(work: *mut IrqWork) {
    wakeup_inband_waiters(work);
}
fn wakeup_inband_waiters(work: *mut IrqWork) {
    let stax = unsafe { &mut *(kernel::container_of!(work, RrosStax, irq_work) as *mut RrosStax) };
    stax.inband_wait
        .wake_up(bindings::TASK_NORMAL, 0, 0 as *mut c_void);
}
