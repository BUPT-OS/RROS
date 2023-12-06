// #![feature(allocator_api)]
// #![allow(warnings, unused)]
// #![feature(stmt_expr_attributes)]

#![allow(warnings, unused)]
#![feature(stmt_expr_attributes)]
use crate::factory;
use crate::list::*;
use crate::sched::__rros_timespec;
use crate::sched::{rros_cpu_rq, this_rros_rq, RQ_TDEFER, RQ_TIMER, RQ_TPROXY};
use crate::thread::T_ROOT;
use crate::tick;
use crate::timeout::RROS_INFINITE;
use crate::{
    factory::RrosElement, factory::RrosFactory, factory::RustFile, lock::*, tick::*, timer::*,
    RROS_OOB_CPUS,
};
use alloc::rc::Rc;
use core::borrow::{Borrow, BorrowMut};
use core::cell::RefCell;
use core::cell::UnsafeCell;
use core::clone::Clone;
use core::ops::Deref;
use core::ops::DerefMut;
use core::{mem::align_of, mem::size_of, todo};
use factory::RROS_CLONE_PUBLIC;
use kernel::{
    bindings, c_types, cpumask, double_linked_list::*, file_operations::FileOperations, ktime::*,
    percpu, prelude::*, premmpt, spinlock_init, str::CStr, sync::Lock, sync::SpinLock, sysfs,
    timekeeping,
};
use kernel::io_buffer::IoBufferWriter;
use kernel::file::File;

static mut CLOCKLIST_LOCK: SpinLock<i32> = unsafe { SpinLock::new(1) };

const CONFIG_RROS_LATENCY_USER: KtimeT = 0; //这里先定义为常量，后面应该从/dev/rros中读取
const CONFIG_RROS_LATENCY_KERNEL: KtimeT = 0;
const CONFIG_RROS_LATENCY_IRQ: KtimeT = 0;

const CONFIG_RROS_NR_CLOCKS: usize = 8;

#[derive(Default)]
pub struct RustFileClock;

impl FileOperations for RustFileClock {
    kernel::declare_file_operations!();
}

pub struct RrosClockGravity {
    irq: KtimeT,
    kernel: KtimeT,
    user: KtimeT,
}

impl RrosClockGravity {
    pub fn new(irq: KtimeT, kernel: KtimeT, user: KtimeT) -> Self {
        RrosClockGravity { irq, kernel, user }
    }
    pub fn get_irq(&self) -> KtimeT {
        self.irq
    }

    pub fn get_kernel(&self) -> KtimeT {
        self.kernel
    }

    pub fn get_user(&self) -> KtimeT {
        self.user
    }

    pub fn set_irq(&mut self, irq: KtimeT) {
        self.irq = irq;
    }

    pub fn set_kernel(&mut self, kernel: KtimeT) {
        self.kernel = kernel;
    }

    pub fn set_user(&mut self, user: KtimeT) {
        self.user = user;
    }
}

//定义时钟操作
pub struct RrosClockOps {
    read: Option<fn(&RrosClock) -> KtimeT>,
    readcycles: Option<fn(&RrosClock) -> u64>,
    set: Option<fn(&mut RrosClock, KtimeT) -> i32>, //int
    programlocalshot: Option<fn(&RrosClock)>,
    programremoteshot: Option<fn(&RrosClock, *mut RrosRq)>,
    setgravity: Option<fn(&mut RrosClock, RrosClockGravity)>,
    resetgravity: Option<fn(&mut RrosClock)>,
    adjust: Option<fn(&mut RrosClock)>,
}

impl RrosClockOps {
    pub fn new(
        read: Option<fn(&RrosClock) -> KtimeT>,
        readcycles: Option<fn(&RrosClock) -> u64>,
        set: Option<fn(&mut RrosClock, KtimeT) -> i32>, //int
        programlocalshot: Option<fn(&RrosClock)>,
        programremoteshot: Option<fn(&RrosClock, *mut RrosRq)>,
        setgravity: Option<fn(&mut RrosClock, RrosClockGravity)>,
        resetgravity: Option<fn(&mut RrosClock)>,
        adjust: Option<fn(&mut RrosClock)>,
    ) -> Self {
        RrosClockOps {
            read,
            readcycles,
            set,
            programlocalshot,
            programremoteshot,
            setgravity,
            resetgravity,
            adjust,
        }
    }
}

pub struct RrosClock {
    resolution: KtimeT,
    gravity: RrosClockGravity,
    name: &'static CStr,
    flags: i32,
    ops: RrosClockOps,
    timerdata: *mut RrosTimerbase,
    master: *mut RrosClock,
    offset: KtimeT,
    next: *mut list_head,
    element: Option<Rc<RefCell<RrosElement>>>,
    dispose: Option<fn(&mut RrosClock)>,
    #[cfg(CONFIG_SMP)]
    pub affinity: Option<cpumask::CpumaskT>,
} //____cacheline_aligned

//RrosClock主方法
impl RrosClock {
    pub fn new(
        resolution: KtimeT,
        gravity: RrosClockGravity,
        name: &'static CStr,
        flags: i32,
        ops: RrosClockOps,
        timerdata: *mut RrosTimerbase,
        master: *mut RrosClock,
        offset: KtimeT,
        next: *mut list_head,
        element: Option<Rc<RefCell<RrosElement>>>,
        dispose: Option<fn(&mut RrosClock)>,
        #[cfg(CONFIG_SMP)] affinity: Option<cpumask::CpumaskT>,
    ) -> Self {
        RrosClock {
            resolution,
            gravity,
            name,
            flags,
            ops,
            timerdata,
            master,
            offset,
            next,
            element,
            dispose,
            #[cfg(CONFIG_SMP)]
            affinity,
        }
    }
    pub fn read(&self) -> KtimeT {
        //错误处理
        if self.ops.read.is_some() {
            return self.ops.read.unwrap()(&self);
        }
        return 0;
    }
    pub fn read_cycles(&self) -> u64 {
        //错误处理
        if self.ops.readcycles.is_some() {
            return self.ops.readcycles.unwrap()(&self);
        }
        return 0;
    }
    pub fn set(&mut self, time: KtimeT) -> Result<usize> {
        if self.ops.set.is_some() {
            self.ops.set.unwrap()(self, time);
        } else {
            return Err(kernel::Error::EFAULT); //阻止函数为null情况的执行
        }
        Ok(0)
    }
    pub fn program_local_shot(&self) {
        if self.ops.programlocalshot.is_some() {
            self.ops.programlocalshot.unwrap()(self);
        }
    }
    pub fn program_remote_shot(&self, rq: *mut RrosRq) {
        if self.ops.programremoteshot.is_some() {
            self.ops.programremoteshot.unwrap()(self, rq);
        }
    }
    pub fn set_gravity(&mut self, gravity: RrosClockGravity) {
        if self.ops.setgravity.is_some() {
            self.ops.setgravity.unwrap()(self, gravity);
        }
    }
    pub fn reset_gravity(&mut self) {
        if self.ops.resetgravity.is_some() {
            self.ops.resetgravity.unwrap()(self);
        }
    }
    pub fn adjust(&mut self) {
        if self.ops.adjust.is_some() {
            self.ops.adjust.unwrap()(self);
        }
    }
    pub fn get_timerdata_addr(&self) -> *mut RrosTimerbase {
        //错误处理
        return self.timerdata as *mut RrosTimerbase;
    }

    pub fn get_gravity_irq(&self) -> KtimeT {
        self.gravity.get_irq()
    }

    pub fn get_gravity_kernel(&self) -> KtimeT {
        self.gravity.get_kernel()
    }

    pub fn get_gravity_user(&self) -> KtimeT {
        self.gravity.get_user()
    }

    pub fn get_offset(&self) -> KtimeT {
        self.offset
    }

    pub fn get_master(&self) -> *mut RrosClock {
        self.master
    }
}

//测试通过
pub fn adjust_timer(
    clock: &RrosClock,
    timer: Arc<SpinLock<RrosTimer>>,
    tq: &mut List<Arc<SpinLock<RrosTimer>>>,
    delta: KtimeT,
) {
    let date = timer.lock().get_date();
    timer.lock().set_date(ktime_sub(date, delta));
    let is_periodic = timer.lock().is_periodic();
    if is_periodic == false {
        rros_enqueue_timer(timer.clone(), tq);
        return;
    }

    let start_date = timer.lock().get_start_date();
    timer.lock().set_start_date(ktime_sub(start_date, delta));

    let period = timer.lock().get_interval();
    let diff = ktime_sub(clock.read(), rros_get_timer_expiry(timer.clone()));

    if (diff >= period) {
        let div = ktime_divns(diff, ktime_to_ns(period));
        let periodic_ticks = timer.lock().get_periodic_ticks();
        timer
            .lock()
            .set_periodic_ticks((periodic_ticks as i64 + div) as u64);
    } else if (ktime_to_ns(delta) < 0
        && (timer.lock().get_status() & RROS_TIMER_FIRED != 0)
        && ktime_to_ns(ktime_add(diff, period)) <= 0)
    {
        /*
         * Timer is periodic and NOT waiting for its first
         * shot, so we make it tick sooner than its original
         * date in order to avoid the case where by adjusting
         * time to a sooner date, real-time periodic timers do
         * not tick until the original date has passed.
         */
        let div = ktime_divns(-diff, ktime_to_ns(period));
        let periodic_ticks = timer.lock().get_periodic_ticks();
        let pexpect_ticks = timer.lock().get_pexpect_ticks();
        timer
            .lock()
            .set_periodic_ticks((periodic_ticks as i64 - div) as u64);
        timer
            .lock()
            .set_pexpect_ticks((pexpect_ticks as i64 - div) as u64);
    }
    rros_update_timer_date(timer.clone());
    rros_enqueue_timer(timer.clone(), tq);
}

//简单测过
//调整当前clock各个CPU tmb中List中的所有timer
pub fn rros_adjust_timers(clock: &mut RrosClock, delta: KtimeT) {
    //raw_spin_lock_irqsave(&tmb->lock, flags);
    let cpu = 0;
    //for_each_online_cpu(cpu) {
    let rq = rros_cpu_rq(cpu);
    let tmb = rros_percpu_timers(clock, cpu);
    let tq = unsafe { &mut (*tmb).q };

    for i in 1..=tq.len() {
        let timer = tq.get_by_index(i).unwrap().value.clone();
        let get_clock = timer.lock().get_clock();
        if get_clock == clock as *mut RrosClock {
            rros_dequeue_timer(timer.clone(), tq);
            adjust_timer(clock, timer.clone(), tq, delta);
        }
    }

    if rq != this_rros_rq() {
        rros_program_remote_tick(clock, rq);
    } else {
        rros_program_local_tick(clock);
    }
    //}
}

//测试通过
pub fn rros_stop_timers(clock: &RrosClock) {
    let cpu = 0;
    let mut tmb = rros_percpu_timers(&clock, cpu);
    let tq = unsafe { &mut (*tmb).q };
    while tq.is_empty() == false {
        //raw_spin_lock_irqsave(&tmb->lock, flags);
        pr_info!("rros_stop_timers: 213");
        let timer = tq.get_head().unwrap().value.clone();
        rros_timer_deactivate(timer);
        //raw_spin_unlock_irqrestore(&tmb->lock, flags);
    }
}

/*
 void inband_clock_was_set(void)
{
    struct rros_clock *clock;

    if (!rros_is_enabled())
        return;

    mutex_lock(&clocklist_lock);

    list_for_each_entry(clock, &clock_list, next) {
        if (clock->ops.adjust)
            clock->ops.adjust(clock);
    }

    mutex_unlock(&clocklist_lock);
}
 */

//打印clock的初始化log
fn rros_clock_log() {}

/*mono时钟操作 */

fn read_mono_clock(clock: &RrosClock) -> KtimeT {
    timekeeping::ktime_get_mono_fast_ns()
}

fn read_mono_clock_cycles(clock: &RrosClock) -> u64 {
    read_mono_clock(clock) as u64
}

fn set_mono_clock(clock: &mut RrosClock, time: KtimeT) -> i32 {
    //mono无法设置,后面应该为错误类型
    0
}

fn adjust_mono_clock(clock: &mut RrosClock) {}

/*realtime时钟操作 */

fn read_realtime_clock(clock: &RrosClock) -> KtimeT {
    timekeeping::ktime_get_real_fast_ns()
}

fn read_realtime_clock_cycles(clock: &RrosClock) -> u64 {
    read_realtime_clock(clock) as u64
}

fn set_realtime_clock(clock: &mut RrosClock, time: KtimeT) -> i32 {
    0
}

fn adjust_realtime_clock(clock: &mut RrosClock) {
    // let old_offset: KtimeT = clock.offset;
    // unsafe {
    //     clock.offset = RROS_REALTIME_CLOCK.read() - RROS_MONO_CLOCK.read();
    // }
    // rros_adjust_timers(clock, clock.offset - old_offset)
}

/*通用clock操作 */

fn get_default_gravity() -> RrosClockGravity {
    RrosClockGravity {
        irq: CONFIG_RROS_LATENCY_IRQ,
        kernel: CONFIG_RROS_LATENCY_KERNEL,
        user: CONFIG_RROS_LATENCY_USER,
    }
}

fn set_coreclk_gravity(clock: &mut RrosClock, gravity: RrosClockGravity) {
    clock.gravity.irq = gravity.irq;
    clock.gravity.kernel = gravity.kernel;
    clock.gravity.user = gravity.user;
}

fn reset_coreclk_gravity(clock: &mut RrosClock) {
    set_coreclk_gravity(clock, get_default_gravity());
}

//两个全局变量MONO和REALTIME
static RROS_MONO_CLOCK_NAME: &CStr =
    unsafe { CStr::from_bytes_with_nul_unchecked("RROS_CLOCK_MONOTONIC_DEV\0".as_bytes()) };
pub static mut RROS_MONO_CLOCK: RrosClock = RrosClock {
    name: RROS_MONO_CLOCK_NAME,
    resolution: 1,
    gravity: RrosClockGravity {
        irq: CONFIG_RROS_LATENCY_IRQ,
        kernel: CONFIG_RROS_LATENCY_KERNEL,
        user: CONFIG_RROS_LATENCY_USER,
    },
    flags: RROS_CLONE_PUBLIC,
    ops: RrosClockOps {
        read: Some(read_mono_clock),
        readcycles: Some(read_mono_clock_cycles),
        set: None,
        programlocalshot: Some(rros_program_proxy_tick),
        #[cfg(CONFIG_SMP)]
        programremoteshot: Some(rros_send_timer_ipi),
        #[cfg(not(CONFIG_SMP))]
        programremoteshot: None,
        setgravity: Some(set_coreclk_gravity),
        resetgravity: Some(reset_coreclk_gravity),
        adjust: None,
    },
    timerdata: 0 as *mut RrosTimerbase,
    master: 0 as *mut RrosClock,
    next: 0 as *mut list_head,
    offset: 0,
    element: None,
    dispose: None,
    #[cfg(CONFIG_SMP)]
    affinity: None,
};

static RROS_REALTIME_CLOCK_NAME: &CStr =
    unsafe { CStr::from_bytes_with_nul_unchecked("RROS_CLOCK_REALTIME_DEV\0".as_bytes()) };
pub static mut RROS_REALTIME_CLOCK: RrosClock = RrosClock {
    name: RROS_REALTIME_CLOCK_NAME,
    resolution: 1,
    gravity: RrosClockGravity {
        irq: CONFIG_RROS_LATENCY_IRQ,
        kernel: CONFIG_RROS_LATENCY_KERNEL,
        user: CONFIG_RROS_LATENCY_USER,
    },
    flags: RROS_CLONE_PUBLIC,
    ops: RrosClockOps {
        read: Some(read_realtime_clock),
        readcycles: Some(read_realtime_clock_cycles),
        set: None,
        programlocalshot: None,
        programremoteshot: None,
        setgravity: Some(set_coreclk_gravity),
        resetgravity: Some(reset_coreclk_gravity),
        adjust: Some(adjust_realtime_clock),
    },
    timerdata: 0 as *mut RrosTimerbase,
    master: 0 as *mut RrosClock,
    next: 0 as *mut list_head,
    offset: 0,
    dispose: None,
    element: None,
    #[cfg(CONFIG_SMP)]
    affinity: None,
};

pub static mut CLOCK_LIST: List<*mut RrosClock> = List::<*mut RrosClock> {
    head: Node::<*mut RrosClock> {
        next: None,
        prev: None,
        value: 0 as *mut RrosClock,
    },
};

/*
struct rros_factory rros_clock_factory = {
    .name	=	RROS_CLOCK_DEV,
    .fops	=	&clock_fops,
    .nrdev	=	CONFIG_RROS_NR_CLOCKS,
    .attrs	=	clock_groups,
    .dispose =	clock_factory_dispose,
};
*/
pub static mut RROS_CLOCK_FACTORY: SpinLock<factory::RrosFactory> = unsafe {
    SpinLock::new(factory::RrosFactory {
        name: unsafe { CStr::from_bytes_with_nul_unchecked("RROS_CLOCK_DEV\0".as_bytes()) },
        // fops: Some(&Clockops),
        nrdev: CONFIG_RROS_NR_CLOCKS,
        build: None,
        dispose: Some(clock_factory_dispose),
        attrs: None, //sysfs::attribute_group::new(),
        flags: 2,
        inside: Some(factory::RrosFactoryInside {
            rrtype: None,
            class: None,
            cdev: None,
            device: None,
            sub_rdev: None,
            kuid: None,
            kgid: None,
            minor_map: None,
            index: None,
            name_hash: None,
            hash_lock: None,
            register: None,
        }),
    })
};

struct Clockops;

impl FileOperations for Clockops {
    kernel::declare_file_operations!(read);

    fn read<T: IoBufferWriter>(
        _this: &Self,
        _file: &File,
        _data: &mut T,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("I'm the read ops of the rros clock factory.");
        Ok(1)
    }
}

pub fn clock_factory_dispose(ele: factory::RrosElement) {}

/*
void rros_core_tick(struct clock_event_device *dummy) /* hard irqs off */
{
    struct rros_rq *this_rq = this_rros_rq();
    struct rros_timerbase *tmb;

    if (RROS_WARN_ON_ONCE(CORE, !is_rros_cpu(rros_rq_cpu(this_rq))))
        return;

    tmb = rros_this_cpu_timers(&rros_mono_clock);
    do_clock_tick(&rros_mono_clock, tmb);

    /*
     * If an RROS thread was preempted by this clock event, any
     * transition to the in-band context will cause a pending
     * in-band tick to be propagated by rros_schedule() called from
     * rros_exit_irq(), so we may have to propagate the in-band
     * tick immediately only if the in-band context was preempted.
     */
    if ((this_rq->local_flags & RQ_TPROXY) && (this_rq->curr->state & T_ROOT))
        rros_notify_proxy_tick(this_rq);
}
*/

fn timer_needs_enqueuing(timer: *mut RrosTimer) -> bool {
    unsafe {
        return ((*timer).get_status()
            & (RROS_TIMER_PERIODIC
                | RROS_TIMER_DEQUEUED
                | RROS_TIMER_RUNNING
                | RROS_TIMER_KILLED))
            == (RROS_TIMER_PERIODIC | RROS_TIMER_DEQUEUED | RROS_TIMER_RUNNING);
    }
}

//rq相关未测试，其余测试通过
pub fn do_clock_tick(clock: &mut RrosClock, tmb: *mut RrosTimerbase) {
    let rq = this_rros_rq();
    // #[cfg(CONFIG_RROS_DEBUG_CORE)]
    // if hard_irqs_disabled() == false {
    //     hard_local_irq_disable();
    // }
    let mut tq = unsafe { &mut (*tmb).q };
    //unsafe{(*tmb).lock.lock();}

    unsafe {
        (*rq).add_local_flags(RQ_TIMER);
    }

    let mut now = clock.read();

    // unsafe{
    //     if (*tmb).q.is_empty() == true {
    //         // tick
    //         tick::proxy_set_next_ktime(1000000, 0 as *mut bindings::clock_event_device);
    //     }
    // }

    unsafe {
        while tq.is_empty() == false {
            let mut timer = tq.get_head().unwrap().value.clone();
            let date = (*timer.locked_data().get()).get_date();
            if now < date {
                break;
            }

            rros_dequeue_timer(timer.clone(), tq);

            rros_account_timer_fired(timer.clone());
            (*timer.locked_data().get()).add_status(RROS_TIMER_FIRED);
            let timer_addr = timer.locked_data().get();

            let inband_timer_addr = (*rq).get_inband_timer().locked_data().get();
            if (timer_addr == inband_timer_addr) {
                (*rq).add_local_flags(RQ_TPROXY);
                (*rq).change_local_flags(!RQ_TDEFER);
                continue;
            }
            let handler = (*timer.locked_data().get()).get_handler();
            let c_ref = timer.locked_data().get();
            handler(c_ref);
            now = clock.read();
            let var_timer_needs_enqueuing = timer_needs_enqueuing(timer.locked_data().get());
            if var_timer_needs_enqueuing == true {
                loop {
                    let periodic_ticks = (*timer.locked_data().get()).get_periodic_ticks() + 1;
                    (*timer.locked_data().get()).set_periodic_ticks(periodic_ticks);
                    rros_update_timer_date(timer.clone());

                    let date = (*timer.locked_data().get()).get_date();
                    if date >= now {
                        break;
                    }
                }

                if (rros_timer_on_rq(timer.clone(), rq)) {
                    rros_enqueue_timer(timer.clone(), tq);
                }

                pr_info!("now is {}", now);
                // pr_info!("date is {}",timer.lock().get_date());
            }
        }
    }
    unsafe { (*rq).change_local_flags(!RQ_TIMER) };

    rros_program_local_tick(clock as *mut RrosClock);

    //raw_spin_unlock(&tmb->lock);
}

#[no_mangle]
pub unsafe extern "C" fn rros_core_tick(dummy: *mut bindings::clock_event_device) {
    // pr_info!("in rros_core_tick");
    let this_rq = this_rros_rq();
    //	if (RROS_WARN_ON_ONCE(CORE, !is_rros_cpu(rros_rq_cpu(this_rq))))
    // pr_info!("in rros_core_tick");
    unsafe {
        do_clock_tick(&mut RROS_MONO_CLOCK, rros_this_cpu_timers(&RROS_MONO_CLOCK));

        let rq_has_tproxy = ((*this_rq).local_flags & RQ_TPROXY != 0x0);
        let assd = (*(*this_rq).get_curr().locked_data().get()).state;
        let curr_state_is_t_root = (assd & (T_ROOT as u32) != 0x0);
        //这个if进不去有问题！！
        // let a = ((*this_rq).local_flags & RQ_TPROXY != 0x0);
        // if rq_has_tproxy  {
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        // }
        // let b = ((*this_rq).get_curr().lock().deref_mut().state & (T_ROOT as u32) != 0x0);

        // if curr_state_is_t_root  {
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        //     pr_info!("in rros_core_tick");
        // }
        if rq_has_tproxy && curr_state_is_t_root {
            rros_notify_proxy_tick(this_rq);
        }
    }
}

//初始化时钟
fn init_clock(clock: *mut RrosClock, master: *mut RrosClock) -> Result<usize> {
    // unsafe{
    //     if (*clock).element.is_none(){
    //         return Err(kernel::Error::EINVAL);
    //     }
    // }
    // unsafe{
    //     factory::rros_init_element((*clock).element.as_ref().unwrap().clone(),
    //     &mut RROS_CLOCK_FACTORY, (*clock).flags & RROS_CLONE_PUBLIC);
    // }
    unsafe {
        (*clock).master = master;
    }
    //rros_create_core_element_device()?;

    unsafe {
        CLOCKLIST_LOCK.lock();
        CLOCK_LIST.add_head(clock);
        CLOCKLIST_LOCK.unlock();
    }

    Ok(0)
}

//初始化时钟slave
fn rros_init_slave_clock(clock: &mut RrosClock, master: &mut RrosClock) -> Result<usize> {
    premmpt::running_inband()?;

    //这里为什么会报错，timer就可以跑？为什么卧槽
    // #[cfg(CONFIG_SMP)]
    // clock.affinity = master.affinity;

    clock.timerdata = master.get_timerdata_addr();
    clock.offset = clock.read() - master.read();
    init_clock(clock as *mut RrosClock, master as *mut RrosClock)?;
    Ok(0)
}

//rros初始化时钟
fn rros_init_clock(clock: &mut RrosClock, affinity: &cpumask::CpumaskT) -> Result<usize> {
    premmpt::running_inband()?;
    let tmb = percpu::alloc_per_cpu(
        size_of::<RrosTimerbase>() as usize,
        align_of::<RrosTimerbase>() as usize,
    ) as *mut RrosTimerbase; //8字节对齐
    if tmb == 0 as *mut RrosTimerbase {
        return Err(kernel::Error::ENOMEM);
    }
    clock.timerdata = tmb;

    let mut tmb = rros_percpu_timers(clock, 0);

    unsafe {
        raw_spin_lock_init(&mut (*tmb).lock);
    }

    clock.offset = 0;
    let ret = init_clock(clock as *mut RrosClock, clock as *mut RrosClock);
    if let Err(_) = ret {
        percpu::free_per_cpu(clock.get_timerdata_addr() as *mut u8);
        return ret;
    }
    Ok(0)
}

//时钟系统初始化
pub fn rros_clock_init() -> Result<usize> {
    let pinned = unsafe { Pin::new_unchecked(&mut CLOCKLIST_LOCK) };
    spinlock_init!(pinned, "CLOCKLIST_LOCK");
    unsafe {
        RROS_MONO_CLOCK.reset_gravity();
        RROS_REALTIME_CLOCK.reset_gravity();
        rros_init_clock(&mut RROS_MONO_CLOCK, &RROS_OOB_CPUS)?;
    }
    let ret = unsafe { rros_init_slave_clock(&mut RROS_REALTIME_CLOCK, &mut RROS_MONO_CLOCK) };
    if let Err(_) = ret {
        //rros_put_element(&rros_mono_clock.element);
    }
    pr_info!("clock init success!");
    Ok(0)
}

pub fn rros_read_clock(clock: &RrosClock) -> KtimeT {
    let clock_add = clock as *const RrosClock;
    let mono_add = unsafe { &RROS_MONO_CLOCK as *const RrosClock };

    if (clock_add == mono_add) {
        return rros_ktime_monotonic();
    }

    clock.ops.read.unwrap()(&clock)
}

fn rros_ktime_monotonic() -> KtimeT {
    timekeeping::ktime_get_mono_fast_ns()
}

// static inline ktime_t rros_read_clock(struct rros_clock *clock)
// {
// 	/*
// 	 * In many occasions on the fast path, rros_read_clock() is
// 	 * explicitly called with &rros_mono_clock which resolves as
// 	 * a constant. Skip the clock trampoline handler, branching
// 	 * immediately to the final code for such clock.
// 	 */
// 	if (clock == &rros_mono_clock)
// 		return rros_ktime_monotonic();

// 	return clock->ops.read(clock);
// }


pub fn u_timespec_to_ktime(u_ts:__rros_timespec) -> KtimeT{
    extern "C"{
        fn rust_helper_timespec64_to_ktime(ts:bindings::timespec64) -> KtimeT;
    }
    let ts64 = bindings::timespec64{
        tv_sec:u_ts.tv_sec as i64,
        tv_nsec:u_ts.tv_nsec as i64,
    };
    
    unsafe{
        rust_helper_timespec64_to_ktime(ts64)
    }
}