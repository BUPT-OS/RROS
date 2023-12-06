use crate::flags::RrosFlag;
use crate::wait::{RrosWaitChannel, RrosWaitQueue, RROS_WAIT_PRIO};
use crate::{idle, sched, tp};
use alloc::rc::Rc;
use kernel::linked_list::{GetLinks, Links};
use core::cell::RefCell;
use core::default::Default;
use core::ops::DerefMut;
use core::ptr::NonNull;
use core::ptr::{null, null_mut};

#[warn(unused_mut)]
use kernel::{
    bindings, c_types, cpumask,c_str,
    double_linked_list::*,
    irq_work::IrqWork,
    ktime::*,
    percpu::alloc_per_cpu,
    percpu_defs,
    percpu_defs::per_cpu,
    prelude::*,
    premmpt, spinlock_init,
    str::CStr,
    sync::{Guard, Lock, SpinLock},
};

use core::mem::{align_of, size_of, transmute};
// use crate::weak;
use crate::{
    fifo, thread::*, timer::*, clock::{self,RrosClock},list,
    timeout::RROS_INFINITE, tick,stat, lock,factory::RrosElement,
};

extern "C" {
    fn rust_helper_cpumask_of(cpu: i32) -> *const cpumask::CpumaskT;
    fn rust_helper_list_add_tail(new: *mut list_head, head: *mut list_head);
    fn rust_helper_dovetail_current_state() -> *mut bindings::oob_thread_state;
    fn rust_helper_test_bit(nr: i32, addr: *const u32) -> bool;
}

pub const RQ_SCHED: u64 = 0x10000000;
pub const RQ_TIMER: u64 = 0x00010000;
pub const RQ_TPROXY: u64 = 0x00008000;
pub const RQ_IRQ: u64 = 0x00004000;
pub const RQ_TDEFER: u64 = 0x00002000;
pub const RQ_IDLE: u64 = 0x00001000;
pub const RQ_TSTOPPED: u64 = 0x00000800;

pub const SCHED_WEAK: i32 = 43;
pub const SCHED_IDLE: i32 = 5;
pub const SCHED_FIFO: i32 = 1;
pub const SCHED_RR: i32 = 2;
pub const SCHED_TP: i32 = 45;
pub const RROS_CLASS_WEIGHT_FACTOR: i32 = 1024;
pub const RROS_MM_PTSYNC_BIT: i32 = 0;

static mut rros_sched_topmost: *mut rros_sched_class = 0 as *mut rros_sched_class;
static mut rros_sched_lower: *mut rros_sched_class = 0 as *mut rros_sched_class;

// static mut rros_thread_list: List<Arc<SpinLock<rros_thread>>> = ;

// pub static mut rros_sched_topmost:*mut rros_sched_class = 0 as *mut rros_sched_class;
// pub static mut rros_sched_lower:*mut rros_sched_class = 0 as *mut rros_sched_class;

//#[derive(Copy,Clone)]
#[repr(C)]
pub struct rros_rq {
    pub flags: u64,
    pub curr: Option<Arc<SpinLock<rros_thread>>>,
    pub fifo: rros_sched_fifo,
    pub weak: rros_sched_weak,
    pub tp: tp::Rros_sched_tp,
    pub root_thread: Option<Arc<SpinLock<rros_thread>>>,
    pub local_flags: u64,
    pub inband_timer: Option<Arc<SpinLock<rros_timer>>>,
    pub rrbtimer: Option<Arc<SpinLock<rros_timer>>>,
    pub proxy_timer_name: *mut c_types::c_char,
    pub rrb_timer_name: *mut c_types::c_char,
    #[cfg(CONFIG_SMP)]
    pub cpu: i32,
    #[cfg(CONFIG_SMP)]
    pub resched_cpus: cpumask::CpumaskT,
    #[cfg(CONFIG_RROS_RUNSTATS)]
    pub last_account_switch: KtimeT,
    #[cfg(CONFIG_RROS_RUNSTATS)]
    pub current_account: *mut stat::RrosAccount,
    pub lock: bindings::hard_spinlock_t,
}

impl rros_rq {
    pub fn new() -> Result<Self> {
        Ok(rros_rq {
            flags: 0,
            curr: None,
            fifo: rros_sched_fifo::new()?,
            weak: rros_sched_weak::new(),
            tp: tp::Rros_sched_tp::new()?,
            root_thread: None,
            // root_thread: unsafe{Some(Arc::try_new(SpinLock::new(rros_thread::new()?))?)},
            local_flags: 0,
            inband_timer: None,
            rrbtimer: None,
            proxy_timer_name: null_mut(),
            rrb_timer_name: null_mut(),
            #[cfg(CONFIG_SMP)]
            cpu: 0,
            #[cfg(CONFIG_SMP)]
            resched_cpus: cpumask::CpumaskT::from_int(0 as u64),
            #[cfg(CONFIG_RROS_RUNSTATS)]
            last_account_switch: 0,
            #[cfg(CONFIG_RROS_RUNSTATS)]
            current_account: stat::RrosAccount::new() as *mut stat::RrosAccount,
            lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map {
                    wait_type_outer: 0,
                    wait_type_inner: 0,
                },
            },
        })
    }

    pub fn get_inband_timer(&self) -> Arc<SpinLock<rros_timer>> {
        self.inband_timer.as_ref().unwrap().clone()
    }

    pub fn get_rrbtimer(&self) -> Arc<SpinLock<rros_timer>> {
        self.rrbtimer.as_ref().unwrap().clone()
    }

    pub fn get_curr(&self) -> Arc<SpinLock<rros_thread>> {
        self.curr.as_ref().unwrap().clone()
    }

    pub fn add_local_flags(&mut self, local_flag: u64) {
        self.local_flags |= local_flag;
    }

    pub fn change_local_flags(&mut self, local_flag: u64) {
        self.local_flags &= local_flag;
    }

    pub fn get_local_flags(&self) -> u64 {
        self.local_flags
    }

    pub fn add_flags(&mut self, flags: u64) {
        self.flags |= flags;
    }

    pub fn get_cpu(&self) -> i32 {
        self.cpu
    }
}

#[no_mangle]
pub static helloworldint: i32 = 5433;

static mut rros_runqueues: *mut rros_rq = 0 as *mut rros_rq;

pub static RROS_CPU_AFFINITY: cpumask::CpumaskT = cpumask::CpumaskT::from_int(0 as u64);

pub fn rros_cpu_rq(cpu: i32) -> *mut rros_rq {
    unsafe { percpu_defs::per_cpu(rros_runqueues, cpu) }
}

pub fn this_rros_rq() -> *mut rros_rq {
    unsafe {
        percpu_defs::per_cpu_ptr(rros_runqueues as *mut u8, percpu_defs::smp_processor_id())
            as *mut rros_rq
    }
}

pub fn this_rros_rq_thread() -> Option<Arc<SpinLock<rros_thread>>> {
    let rq = this_rros_rq();
    unsafe { (*rq).curr.clone() }
}

pub fn rros_need_resched(rq: *mut rros_rq) -> bool {
    unsafe { (*rq).flags & RQ_SCHED != 0x0 }
}

pub fn rros_set_self_resched(rq: Option<*mut rros_rq>) -> Result<usize> {
    match rq {
        Some(r) => unsafe {
            (*r).flags |= RQ_SCHED;
            // (*r).local_flags |= RQ_SCHED;
        },
        None => return Err(kernel::Error::EINVAL),
    }
    Ok(0)
}

//测试通过
#[cfg(CONFIG_SMP)]
pub fn rros_rq_cpu(rq: *mut rros_rq) -> i32 {
    unsafe { (*rq).get_cpu() }
}

#[cfg(not(CONFIG_SMP))]
pub fn rros_rq_cpu(rq: *mut rros_rq) -> i32 {
    return 0;
}

pub fn rros_protect_thread_priority(thread:Arc<SpinLock<rros_thread>>, prio:i32) -> Result<usize>{
    unsafe{
        // raw_spin_lock(&thread->rq->lock);
        let mut state = (*thread.locked_data().get()).state;
        if state & T_READY != 0{
            rros_dequeue_thread(thread.clone());
        }

        (*thread.locked_data().get()).sched_class = Some(&fifo::rros_sched_fifo);
        rros_ceil_priority(thread.clone(), prio);
        
        state = (*thread.locked_data().get()).state;
        if state & T_READY != 0{
            rros_enqueue_thread(thread.clone());
        }

        let rq = (*thread.locked_data().get()).rq;
        rros_set_resched(rq.clone());

        // raw_spin_unlock(&thread->rq->lock);
        Ok(0)
    }
}

//测试通过
#[cfg(CONFIG_SMP)]
pub fn rros_set_resched(rq_op: Option<*mut rros_rq>) {
    let rq;
    match rq_op {
        None => return,
        Some(x) => rq = x,
    };
    let this_rq = this_rros_rq();
    if this_rq == rq {
        unsafe {
            (*this_rq).add_flags(RQ_SCHED);
        }
    } else if rros_need_resched(rq) == false {
        unsafe {
            (*rq).add_flags(RQ_SCHED);
            (*this_rq).add_local_flags(RQ_SCHED);
            cpumask::cpumask_set_cpu(
                rros_rq_cpu(rq) as u32,
                (*this_rq).resched_cpus.as_cpumas_ptr(),
            );
        }
    }
}

#[cfg(not(CONFIG_SMP))]
pub fn rros_set_resched(rq: Option<*mut rros_rq>) {
    rros_set_self_resched(rq_clone)
}

//暂时不用
#[cfg(CONFIG_SMP)]
pub fn is_threading_cpu(cpu: i32) -> bool {
    //return !!cpumask_test_cpu(cpu, &rros_cpu_affinity);
    return false;
}

//测试通过
#[cfg(not(CONFIG_SMP))]
pub fn is_threading_cpu(cpu: i32) -> bool {
    return true;
}

#[cfg(not(CONFIG_SMP))]
pub fn is_rros_cpu(cpu: i32) -> bool {
    return true;
}

//void rros_migrate_thread(struct rros_thread *thread,
//    struct rros_rq *dst_rq);
#[cfg(CONFIG_SMP)]
pub fn rros_migrate_thread(thread: Arc<SpinLock<rros_thread>>, dst_rq: *mut rros_rq) {
    //todo
}

#[cfg(not(CONFIG_SMP))]
pub fn rros_migrate_thread(thread: Arc<SpinLock<rros_thread>>, dst_rq: *mut rros_rq) {}

//简单函数未测试
pub fn rros_in_irq() -> bool {
    let rq = this_rros_rq();
    unsafe { (*rq).get_local_flags() & RQ_IRQ != 0 }
}

//简单函数未测试
pub fn rros_is_inband() -> bool {
    let thread_op = this_rros_rq_thread();
    let state;
    match thread_op {
        None => return false,
        Some(x) => state = x.lock().state,
    }
    state & T_ROOT != 0x0
}

//简单函数未测试
pub fn rros_cannot_block() -> bool {
    rros_in_irq() || rros_is_inband()
}

#[no_mangle]
unsafe extern "C" fn this_rros_rq_enter_irq_local_flags() {
    unsafe {
        if rros_runqueues == 0 as *mut rros_rq {
            return;
        }
    }

    let rq = this_rros_rq();

    unsafe {
        (*rq).local_flags |= RQ_IRQ;
    }
}

#[no_mangle]
unsafe extern "C" fn this_rros_rq_exit_irq_local_flags() -> c_types::c_int {
    unsafe {
        if rros_runqueues == 0 as *mut rros_rq {
            return 0;
        }
    }

    let rq = this_rros_rq();
    // struct rros_rq *rq = this_rros_rq();

    unsafe {
        (*rq).local_flags &= !RQ_IRQ;
    }

    let flags;
    let local_flags;

    unsafe {
        flags = (*rq).flags;
        local_flags = (*rq).local_flags;
    }

    // pr_info!("{} cc {} \n", flags, local_flags);

    if ((flags | local_flags) & RQ_SCHED) != 0x0 {
        return 1 as c_types::c_int;
    }

    0 as c_types::c_int
}

//#[derive(Copy,Clone)]
pub struct rros_sched_fifo {
    pub runnable: rros_sched_queue,
}
impl rros_sched_fifo {
    fn new() -> Result<Self> {
        Ok(rros_sched_fifo {
            runnable: rros_sched_queue::new()?,
        })
    }
}

//#[derive(Copy,Clone)]
pub struct rros_sched_weak {
    pub runnable: Option<Rc<RefCell<rros_sched_queue>>>,
}
impl rros_sched_weak {
    fn new() -> Self {
        rros_sched_weak { runnable: None }
    }
}

// #[derive(Copy,Clone)]
pub struct rros_sched_queue {
    pub head: Option<List<Arc<SpinLock<rros_thread>>>>,
}
impl rros_sched_queue {
    pub fn new() -> Result<Self> {
        Ok(rros_sched_queue {
            head: None,
            // head: unsafe{List::new(Arc::try_new(SpinLock::new(rros_rq::new()?))?)},
        })
    }
}

//#[derive(Copy,Clone)]
// pub type list_head = bindings::list_head;

pub struct list_head {
    pub next: *mut list_head,
    pub prev: *mut list_head,
}
impl list_head {
    pub fn new() -> Self {
        list_head {
            next: 0 as *mut list_head,
            prev: 0 as *mut list_head,
        }
    }
}

pub type ssize_t = bindings::__kernel_ssize_t;

// #[derive(Copy,Clone)]
pub struct rros_sched_class {
    pub sched_init: Option<fn(rq: *mut rros_rq) -> Result<usize>>,
    pub sched_enqueue: Option<fn(thread: Arc<SpinLock<rros_thread>>) -> Result<i32>>,
    pub sched_dequeue: Option<fn(thread: Arc<SpinLock<rros_thread>>)>,
    pub sched_requeue: Option<fn(thread: Arc<SpinLock<rros_thread>>)>,
    pub sched_pick: Option<fn(rq: Option<*mut rros_rq>) -> Result<Arc<SpinLock<rros_thread>>>>,
    pub sched_tick: Option<fn(rq: Option<*mut rros_rq>)-> Result<usize>>,
    pub sched_migrate: Option<fn(thread: Arc<SpinLock<rros_thread>>, rq: *mut rros_rq)->Result<usize>>,
    pub sched_setparam:
        Option<fn(thread: Option<Arc<SpinLock<rros_thread>>>,p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<usize>>,
    pub sched_getparam:
        Option<fn(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>)>,
    pub sched_chkparam:
        Option<fn(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>) -> Result<i32>>,
    pub sched_trackprio: Option<fn(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>)>,
    pub sched_ceilprio: Option<fn(thread: Arc<SpinLock<rros_thread>>, prio: i32)>,

    pub sched_declare:
        Option<fn(thread: Option<Arc<SpinLock<rros_thread>>>,p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<i32>>,
    pub sched_forget: Option<fn(thread:Arc<SpinLock<rros_thread>>) -> Result<usize>>,
    pub sched_kick: Option<fn(thread: Rc<RefCell<rros_thread>>)>,
    pub sched_show: Option<
        fn(
            thread: *mut rros_thread,
            buf: *mut c_types::c_char,
            count: ssize_t,
        ) -> Result<usize>,
    >,
    pub sched_control: Option<
        fn(
            cpu: i32,
            ctlp: *mut rros_sched_ctlparam,
            infp: *mut rros_sched_ctlinfo,
        ) -> Result<ssize_t>,
    >,
    pub nthreads: i32,
    pub next: *mut rros_sched_class,
    pub weight: i32,
    pub policy: i32,
    pub name: &'static str,
    pub flag:i32, // 标识调度类 1:rros_sched_idle 3:rros_sched_fifo 4:rros_sched_tp
}
impl rros_sched_class {
    pub fn new() -> Self {
        rros_sched_class {
            sched_init: None,
            sched_enqueue: None,
            sched_dequeue: None,
            sched_requeue: None,
            sched_pick: None,
            sched_tick: None,
            sched_migrate: None,
            sched_setparam: None,
            sched_getparam: None,
            sched_chkparam: None,
            sched_trackprio: None,
            sched_ceilprio: None,
            sched_declare: None,
            sched_forget: None,
            sched_kick: None,
            sched_show: None,
            sched_control: None,
            nthreads: 0,
            next: 0 as *mut rros_sched_class,
            weight: 0,
            policy: 0,
            name: "sched_class",
            flag: 0,
        }
    }
}

#[derive(Copy, Clone)]
pub struct rros_sched_param {
    pub idle: rros_idle_param,
    pub fifo: rros_fifo_param,
    pub weak: rros_weak_param,
    pub tp: rros_tp_param,
}
impl rros_sched_param {
    pub fn new() -> Self {
        rros_sched_param {
            idle: rros_idle_param::new(),
            fifo: rros_fifo_param::new(),
            weak: rros_weak_param::new(),
            tp:rros_tp_param::new(),
        }
    }
}

#[derive(Copy, Clone)]
pub struct rros_idle_param {
    pub prio: i32,
}
impl rros_idle_param {
    fn new() -> Self {
        rros_idle_param { prio: 0 }
    }
}

#[derive(Copy, Clone)]
pub struct rros_fifo_param {
    pub prio: i32,
}
impl rros_fifo_param {
    fn new() -> Self {
        rros_fifo_param { prio: 0 }
    }
}

#[derive(Copy, Clone)]
pub struct rros_tp_param {
    pub prio:i32,
	pub ptid:i32,	/* partition id. */
}
impl rros_tp_param {
    fn new() -> Self {
        rros_tp_param { prio: 0, ptid:0, }
    }
}

#[derive(Copy, Clone)]
pub struct rros_weak_param {
    pub prio: i32,
}
impl rros_weak_param {
    fn new() -> Self {
        rros_weak_param { prio: 0 }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_sched_ctlparam {
    pub quota: rros_quota_ctlparam,
    pub tp: rros_tp_ctlparam,
}
impl rros_sched_ctlparam {
    fn new() -> Self {
        rros_sched_ctlparam {
            quota: rros_quota_ctlparam::new(),
            tp: rros_tp_ctlparam::new(),
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_sched_ctlinfo {
    pub quota: rros_quota_ctlinfo,
    pub tp: rros_tp_ctlinfo,
}
impl rros_sched_ctlinfo {
    fn new() -> Self {
        rros_sched_ctlinfo {
            quota: rros_quota_ctlinfo::new(),
            tp: rros_tp_ctlinfo::new(),
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_quota_ctlparam {
    pub op: rros_quota_ctlop,
    pub u: u,
}
impl rros_quota_ctlparam {
    fn new() -> Self {
        rros_quota_ctlparam { op: 0, u: u::new() }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_tp_ctlparam {
    pub op: rros_tp_ctlop,
    pub nr_windows: i32,
    pub windows: *mut __rros_tp_window,
}
impl rros_tp_ctlparam {
    fn new() -> Self {
        rros_tp_ctlparam {
            op: 0,
            nr_windows: 0,
            windows: 0 as *mut __rros_tp_window,
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_quota_ctlinfo {
    pub tgid: i32,
    pub quota: i32,
    pub quota_peak: i32,
    pub quota_sum: i32,
}
impl rros_quota_ctlinfo {
    fn new() -> Self {
        rros_quota_ctlinfo {
            tgid: 0,
            quota: 0,
            quota_peak: 0,
            quota_sum: 0,
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_tp_ctlinfo {
    pub nr_windows: i32,
    pub windows: *mut __rros_tp_window,
}
impl rros_tp_ctlinfo {
    fn new() -> Self {
        rros_tp_ctlinfo {
            nr_windows: 0,
            windows: 0 as *mut __rros_tp_window,
        }
    }
}

pub const RROS_QUOTA_CTLOP_RROS_QUOTA_ADD: rros_quota_ctlop = 0;
pub const RROS_QUOTA_CTLOP_RROS_QUOTA_REMOVE: rros_quota_ctlop = 1;
pub const RROS_QUOTA_CTLOP_RROS_QUOTA_FORCE_REMOVE: rros_quota_ctlop = 2;
pub const RROS_QUOTA_CTLOP_RROS_QUOTA_SET: rros_quota_ctlop = 3;
pub const RROS_QUOTA_CTLOP_RROS_QUOTA_GET: rros_quota_ctlop = 4;
pub type rros_quota_ctlop = c_types::c_uint;
pub const RROS_TP_CTLOP_RROS_TP_INSTALL: rros_tp_ctlop = 0;
pub const RROS_TP_CTLOP_RROS_TP_UNINSTALL: rros_tp_ctlop = 1;
pub const RROS_TP_CTLOP_RROS_TP_START: rros_tp_ctlop = 2;
pub const RROS_TP_CTLOP_RROS_TP_STOP: rros_tp_ctlop = 3;
pub const RROS_TP_CTLOP_RROS_TP_GET: rros_tp_ctlop = 4;
pub type rros_tp_ctlop = c_types::c_uint;

//#[derive(Copy,Clone)]
pub struct u {
    pub remove: remove,
    pub set: set,
    pub get: get,
}
impl u {
    fn new() -> Self {
        u {
            remove: remove::new(),
            set: set::new(),
            get: get::new(),
        }
    }
}
//#[derive(Copy,Clone)]
pub struct remove {
    tgid: i32,
}
impl remove {
    fn new() -> Self {
        remove { tgid: 0 }
    }
}
//#[derive(Copy,Clone)]
pub struct set {
    tgid: i32,
    quota: i32,
    quota_peak: i32,
}
impl set {
    fn new() -> Self {
        set {
            tgid: 0,
            quota: 0,
            quota_peak: 0,
        }
    }
}
//#[derive(Copy,Clone)]
pub struct get {
    tgid: i32,
}
impl get {
    fn new() -> Self {
        get { tgid: 0 }
    }
}

//#[derive(Copy,Clone)]
pub struct __rros_tp_window {
    pub offset: *mut __rros_timespec,
    pub duration: *mut __rros_timespec,
    pub ptid: i32,
}
impl __rros_tp_window {
    fn new() -> Self {
        __rros_tp_window {
            offset: 0 as *mut __rros_timespec,
            duration: 0 as *mut __rros_timespec,
            ptid: 0,
        }
    }
}
//#[derive(Copy,Clone)]
pub struct __rros_timespec {
    pub tv_sec: __rros_time64_t,
    pub tv_nsec: c_types::c_longlong,
}
impl __rros_timespec {
    pub fn new() -> Self {
        __rros_timespec {
            tv_sec: 0,
            tv_nsec: 0,
        }
    }
}
pub type __rros_time64_t = c_types::c_longlong;
use crate::timer::RrosTimer as rros_timer;
type ktime_t = i64;
use crate::clock::{RrosClock as rros_clock, RROS_MONO_CLOCK};
use crate::timer::RrosTimerbase as rros_timerbase;

//#[derive(Copy,Clone)]
pub struct rros_tqueue {
    pub q: list_head,
}
impl rros_tqueue {
    fn new() -> Self {
        rros_tqueue {
            q: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
        }
    }
}

//#[derive(Copy,Clone)]
pub struct ops {
    pub read: Option<fn(clock: Rc<RefCell<rros_clock>>) -> ktime_t>,
    pub read_cycles: Option<fn(clock: Rc<RefCell<rros_clock>>) -> u64>,
    pub set: Option<fn(clock: Rc<RefCell<rros_clock>>, date: ktime_t) -> i32>,
    pub program_local_shot: Option<fn(clock: Rc<RefCell<rros_clock>>)>,
    pub program_remote_shot: Option<fn(clock: Rc<RefCell<rros_clock>>, rq: Rc<RefCell<rros_rq>>)>,
    pub set_gravity:
        Option<fn(clock: Rc<RefCell<rros_clock>>, p: *const rros_clock_gravity) -> i32>,
    pub reset_gravity: Option<fn(clock: Rc<RefCell<rros_clock>>)>,
    pub adjust: Option<fn(clock: Rc<RefCell<rros_clock>>)>,
}
impl ops {
    fn new() -> Self {
        ops {
            read: None,
            read_cycles: None,
            set: None,
            program_local_shot: None,
            program_remote_shot: None,
            set_gravity: None,
            reset_gravity: None,
            adjust: None,
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_clock_gravity {
    pub irq: ktime_t,
    pub kernel: ktime_t,
    pub user: ktime_t,
}
impl rros_clock_gravity {
    fn new() -> Self {
        rros_clock_gravity {
            irq: 0,
            kernel: 0,
            user: 0,
        }
    }
}

pub type __u32 = u32;

pub struct rros_stat {
    pub isw: stat::RrosCounter,
    pub csw: stat::RrosCounter,
    pub sc: stat::RrosCounter,
    pub rwa: stat::RrosCounter,
    pub account: stat::RrosAccount,
    pub lastperiod: stat::RrosAccount,
}

impl rros_stat {
    pub fn new() -> Self {
        rros_stat {
            isw: stat::RrosCounter::new(),
            csw: stat::RrosCounter::new(),
            sc: stat::RrosCounter::new(),
            rwa: stat::RrosCounter::new(),
            account: stat::RrosAccount::new(),
            lastperiod: stat::RrosAccount::new(),
        }
    }
}

pub struct RrosThreadWithLock(SpinLock<rros_thread>);
impl RrosThreadWithLock{
    /// transmute back
    pub unsafe fn transmute_to_original(ptr:Arc<Self>) -> Arc<SpinLock<rros_thread>>{
        unsafe{
            let ptr = Arc::into_raw(ptr) as *mut SpinLock<rros_thread>;
            Arc::from_raw(transmute(NonNull::new_unchecked(ptr).as_ptr()))
        }
    }

    pub unsafe fn new_from_curr_thread() -> Arc<Self>{
        unsafe{
            let ptr = transmute(NonNull::new_unchecked(rros_current()).as_ptr());
            let ret = Arc::from_raw(ptr);
            Arc::increment_strong_count(ptr);
            ret
        }
    }
    pub fn get_wprio(&self) -> i32{
       unsafe{
            (*(*self.0.locked_data()).get()).wprio
       } 
    }
}

impl GetLinks for RrosThreadWithLock{
    type EntryType = RrosThreadWithLock;
    fn get_links(data: &Self::EntryType) -> &Links<Self::EntryType> {
        unsafe {
            &(*data.0.locked_data().get()).wait_next
        }  
    }
}

//#[derive(Copy,Clone)]
pub struct rros_thread {
    pub lock: bindings::hard_spinlock_t,

    pub rq: Option<*mut rros_rq>,
    pub base_class: Option<&'static rros_sched_class>,
    pub sched_class: Option<&'static rros_sched_class>,

    pub bprio: i32,
    pub cprio: i32,
    pub wprio: i32,

    // pub boosters: *mut List<Arc<SpinLock<RrosMutex>>>,
    pub wchan: *mut RrosWaitChannel,
    pub wait_next: Links<RrosThreadWithLock>,
    pub wwake: *mut RrosWaitChannel,
    pub rtimer: Option<Arc<SpinLock<rros_timer>>>,
    pub ptimer: Option<Arc<SpinLock<rros_timer>>>,
    pub rrperiod: ktime_t,
    pub state: __u32,
    pub info: __u32,

    // pub rq_next: Option<List<Arc<SpinLock<rros_thread>>>>,
    pub next: *mut Node<Arc<SpinLock<rros_thread>>>,

    pub rq_next: Option<NonNull<Node<Arc<SpinLock<rros_thread>>>>>,

    pub altsched: bindings::dovetail_altsched_context,
    pub local_info: __u32,
    pub wait_data: *mut c_types::c_void,
    pub poll_context: poll_context,

    pub inband_disable_count: bindings::atomic_t,
    pub inband_work: IrqWork,
    pub stat: rros_stat,
    pub u_window: Option<Rc<RefCell<rros_user_window>>>,

    // pub trackers: *mut List<Arc<SpinLock<RrosMutex>>>,
    pub tracking_lock: bindings::hard_spinlock_t,
    pub element: Rc<RefCell<RrosElement>>,
    pub affinity: cpumask::CpumaskT,
    pub exited: bindings::completion,
    pub raised_cap: bindings::kernel_cap_t,
    pub kill_next: list_head,
    pub oob_mm: *mut bindings::oob_mm_state,
    pub ptsync_next: list_head,
    pub observable: Option<Rc<RefCell<rros_observable>>>,
    pub name: &'static str,
    pub tps:*mut tp::rros_tp_rq,
	pub tp_link: Option<Node<Arc<SpinLock<rros_thread>>>>,
}

impl rros_thread {
    pub fn new() -> Result<Self> {
        unsafe{
        Ok(rros_thread {
            lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map {
                    wait_type_outer: 0,
                    wait_type_inner: 0,
                },
            },
            rq: None,
            base_class: None,
            sched_class: None,
            bprio: 0,
            cprio: 0,
            wprio: 0,
            // boosters: 0 as *mut List<Arc<SpinLock<RrosMutex>>>,
            wchan: core::ptr::null_mut(),
            wait_next: Links::new(),
            wwake: core::ptr::null_mut(),
            rtimer: None,
            ptimer: None,
            rrperiod: 0,
            state: 0,
            info: 0,
            // rq_next: unsafe{List::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?)},
            rq_next: None,
            // next: list_head {
            //     next: 0 as *mut list_head,
            //     prev: 0 as *mut list_head,
            // },
            next: 0 as *mut Node<Arc<SpinLock<rros_thread>>>, // kernel corrupted bug
            altsched: bindings::dovetail_altsched_context {
                task: null_mut(),
                active_mm: null_mut(),
                borrowed_mm: false,
            },
            local_info: 0,
            wait_data: null_mut(),
            poll_context: poll_context::new(),
            inband_disable_count: bindings::atomic_t { counter: 0 },
            inband_work: IrqWork::new(),
            stat: rros_stat::new(),
            u_window: None,
            // trackers: 0 as *mut List<Arc<SpinLock<RrosMutex>>>,
            tracking_lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map {
                    wait_type_outer: 0,
                    wait_type_inner: 0,
                },
            },
            element: Rc::try_new(RefCell::new(RrosElement::new()?))?,
            affinity: cpumask::CpumaskT::from_int(0 as u64),
            exited: bindings::completion {
                done: 0,
                wait: bindings::swait_queue_head {
                    lock: bindings::raw_spinlock_t {
                        raw_lock: bindings::arch_spinlock_t {
                            __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                                val: bindings::atomic_t { counter: 0 },
                                // __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1__bindgen_ty_1{
                                // 	locked: 0,
                                // 	pending: 0,
                                //  },
                                // __bindgen_anon_2: bindings::qspinlock__bindgen_ty_1__bindgen_ty_2{
                                // 	locked_pending: 0,
                                // 	tail: 0,
                                // },
                            },
                        },
                    },
                    task_list: bindings::list_head {
                        next: null_mut(),
                        prev: null_mut(),
                    },
                },
            },
            raised_cap: bindings::kernel_cap_t { cap: [0, 0] },
            kill_next: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
            ptsync_next: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
            observable: None,
            name: "thread\0",
            oob_mm:null_mut(),
            tps: 0 as *mut tp::rros_tp_rq,
            tp_link: None,
        })
    }
    }

    pub fn init(&mut self) -> Result<usize>{
        extern "C"{
            fn rust_helper_raw_spin_lock_init(lock: *mut bindings::raw_spinlock_t);
        }
        self.lock = bindings::hard_spinlock_t::default();
        unsafe{
            rust_helper_raw_spin_lock_init(&mut self.lock as *mut bindings::hard_spinlock_t as *mut bindings::raw_spinlock_t);
        }
        self.rq= None;
        self.base_class= None;
        self.sched_class= None;
        self.bprio= 0;
        self.cprio= 0;
        self.wprio= 0;
        self.wchan= core::ptr::null_mut();
        self.wait_next= Links::new();
        self.wwake= core::ptr::null_mut();
        self.rtimer= None;
        self.ptimer= None;
        self.rrperiod= 0;
        self.state= 0;
        self.info= 0;
        self.rq_next= None;
        self.next = 0 as *mut Node<Arc<SpinLock<rros_thread>>>; // kernel;
        self.altsched = bindings::dovetail_altsched_context {
            task: null_mut(),
            active_mm: null_mut(),
            borrowed_mm: false,
        };
        self.local_info = 0;
        self.wait_data = null_mut();
        self.poll_context = poll_context::new();
        self.inband_disable_count = bindings::atomic_t { counter: 0 };
        self.inband_work = IrqWork::new();
        self.stat = rros_stat::new();
        self.u_window = None;
        self.tracking_lock = bindings::hard_spinlock_t::default();
        unsafe{
            rust_helper_raw_spin_lock_init(&mut self.tracking_lock as *mut bindings::hard_spinlock_t as *mut bindings::raw_spinlock_t);
        }
        // self.element = Rc::try_new(RefCell::new(RrosElement::new()?))?;
        self.affinity = cpumask::CpumaskT::from_int(0 as u64);
        self.exited = bindings::completion {
            done: 0,
            wait: bindings::swait_queue_head {
                lock: bindings::raw_spinlock_t {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                            // __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1__bindgen_ty_1{
                            // 	locked: 0,
                            // 	pending: 0,
                            //  },
                            // __bindgen_anon_2: bindings::qspinlock__bindgen_ty_1__bindgen_ty_2{
                            // 	locked_pending: 0,
                            // 	tail: 0,
                            // },
                        },
                    },
                },
                task_list: bindings::list_head {
                    next: null_mut(),
                    prev: null_mut(),
                },
            },
        };
        self.raised_cap = bindings::kernel_cap_t { cap: [0, 0] };
        self.kill_next = list_head {
            next: 0 as *mut list_head,
            prev: 0 as *mut list_head,
        };
        self.ptsync_next = list_head {
            next: 0 as *mut list_head,
            prev: 0 as *mut list_head,
        };
        self.observable = None;
        self.name = "thread\0";
        self.oob_mm =null_mut();
        // self.tps = 0 as *mut tp::rros_tp_rq;
        self.tp_link = None;

        Ok(0)
    }
}

// TODO: move oob_mm_state to c in the mm_info.h
// pub struct oob_mm_state {
//     flags: u32,
//     //todo
//     // struct list_head ptrace_sync;
//     // struct rros_wait_queue ptsync_barrier;
// }
// impl oob_mm_state {
//     fn new() -> Self {
//         oob_mm_state { flags: 0 }
//     }
// }

//#[derive(Copy,Clone)]
pub struct poll_context {
    pub table: Option<Rc<RefCell<rros_poll_watchpoint>>>,
    pub generation: u32,
    pub nr: i32,
    pub active: i32,
}
impl poll_context {
    fn new() -> Self {
        poll_context {
            table: None,
            generation: 0,
            nr: 0,
            active: 0,
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_poll_watchpoint {
    pub fd: u32,
    pub events_polled: i32,
    pub pollval: rros_value,
    // pub wait:oob_poll_wait,  //ifdef
    pub flag: Option<Rc<RefCell<RrosFlag>>>,
    // pub filp:*mut file,
    pub node: rros_poll_node,
}
impl rros_poll_watchpoint {
    fn new() -> Self {
        rros_poll_watchpoint {
            fd: 0,
            events_polled: 0,
            pollval: rros_value::new(),
            flag: None,
            node: rros_poll_node::new(),
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_value {
    pub val: i32,
    pub lval: i64,
    pub ptr: *mut c_types::c_void,
}
impl rros_value {
    fn new() -> Self {
        rros_value {
            val: 0,
            lval: 0,
            ptr: null_mut(),
        }
    }
}

pub struct rros_poll_node {
    pub next: list_head,
}
impl rros_poll_node {
    fn new() -> Self {
        rros_poll_node {
            next: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
        }
    }
}
pub struct rros_user_window {
    pub state: __u32,
    pub info: __u32,
    pub pp_pending: __u32,
}
impl rros_user_window {
    fn new() -> Self {
        rros_user_window {
            state: 0,
            info: 0,
            pp_pending: 0,
        }
    }
}

// #[derive(Copy,Clone)]
pub struct rros_observable {
    // pub element:rros_element,
    pub observers: list_head,
    pub flush_list: list_head,
    pub oob_wait: RrosWaitQueue,
    pub inband_wait: bindings::wait_queue_head_t,
    pub poll_head: rros_poll_head,
    pub wake_irqwork: bindings::irq_work,
    pub flush_irqwork: bindings::irq_work,
    pub lock: bindings::hard_spinlock_t,
    pub serial_counter: u32,
    pub writable_observers: i32,
}
impl rros_observable {
    fn new() -> Self {
        rros_observable {
            observers: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
            flush_list: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
            oob_wait: unsafe{RrosWaitQueue::new(&mut RROS_MONO_CLOCK as *mut RrosClock, RROS_WAIT_PRIO as i32)},
            inband_wait: bindings::wait_queue_head_t {
                lock: bindings::spinlock_t {
                    _bindgen_opaque_blob: 0,
                },
                head: bindings::list_head {
                    next: null_mut(),
                    prev: null_mut(),
                },
            },
            poll_head: rros_poll_head::new(),
            wake_irqwork: bindings::irq_work {
                node: bindings::__call_single_node {
                    llist: bindings::llist_node { next: null_mut() },
                    __bindgen_anon_1: bindings::__call_single_node__bindgen_ty_1 {
                        u_flags: 0,
                        // a_flags:bindings::atomic_t{
                        // 	counter:0,
                        // },
                    },
                    src: 0,
                    dst: 0,
                },
                func: None,
            },
            flush_irqwork: bindings::irq_work {
                node: bindings::__call_single_node {
                    llist: bindings::llist_node { next: null_mut() },
                    __bindgen_anon_1: bindings::__call_single_node__bindgen_ty_1 {
                        u_flags: 0,
                        // a_flags:bindings::atomic_t{
                        // 	counter:0,
                        // },
                    },
                    src: 0,
                    dst: 0,
                },
                func: None,
            },
            lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map {
                    wait_type_outer: 0,
                    wait_type_inner: 0,
                },
            },
            serial_counter: 0,
            writable_observers: 0,
        }
    }
}

//#[derive(Copy,Clone)]
pub struct rros_poll_head {
    pub watchpoints: list_head,
    // FIXME: use ptr here not directly object
    pub lock: bindings::hard_spinlock_t,
}
impl rros_poll_head {
    pub fn new() -> Self {
        rros_poll_head {
            watchpoints: list_head {
                next: 0 as *mut list_head,
                prev: 0 as *mut list_head,
            },
            lock: bindings::hard_spinlock_t {
                rlock: bindings::raw_spinlock {
                    raw_lock: bindings::arch_spinlock_t {
                        __bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
                            val: bindings::atomic_t { counter: 0 },
                        },
                    },
                },
                dep_map: bindings::phony_lockdep_map {
                    wait_type_outer: 0,
                    wait_type_inner: 0,
                },
            },
        }
    }
}

pub struct rros_init_thread_attr {
    pub affinity: *const cpumask::CpumaskT,
    pub observable: Option<Rc<RefCell<rros_observable>>>,
    pub flags: i32,
    pub sched_class: Option<&'static rros_sched_class>,
    pub sched_param: Option<Arc<SpinLock<rros_sched_param>>>,
}
impl rros_init_thread_attr {
    pub fn new() -> Self {
        rros_init_thread_attr {
            affinity: null(),
            observable: None,
            flags: 0,
            sched_class: None,
            sched_param: None,
        }
    }
}
fn init_inband_timer(rq_ptr: *mut rros_rq) -> Result<usize> {
    unsafe{
        (*rq_ptr) = rros_rq::new()?;
        let mut x = SpinLock::new(RrosTimer::new(1));
        let pinned =  Pin::new_unchecked(&mut x);
        spinlock_init!(pinned, "inband_timer");
        (*rq_ptr).inband_timer =  Some(Arc::try_new(x)?);
    }
    Ok(0)
}

fn init_rrbtimer(rq_ptr: *mut rros_rq)  -> Result<usize> {
    unsafe{
        let mut y = SpinLock::new(RrosTimer::new(1));
        let pinned = Pin::new_unchecked(&mut y);
        spinlock_init!(pinned, "rrb_timer");
        (*rq_ptr).rrbtimer =  Some(Arc::try_new(y)?);
    }
    Ok(0)
}

fn init_root_thread(rq_ptr: *mut rros_rq) -> Result<usize> {
    unsafe{
        let mut tmp = Arc::<SpinLock<rros_thread>>::try_new_uninit()?;
        let mut tmp = unsafe{
            core::ptr::write_bytes(Arc::get_mut_unchecked(&mut tmp), 0, 1);
            tmp.assume_init()
        };
        let pinned = unsafe{
           Pin::new_unchecked(Arc::get_mut_unchecked(&mut tmp))
        };
        spinlock_init!(pinned,"rros_kthreads");

        // let mut thread = SpinLock::new(rros_thread::new()?);
        // let pinned = Pin::new_unchecked(&mut thread);
        // spinlock_init!(pinned, "rros_threads");
        // Arc::get_mut(&mut tmp).unwrap().write(thread);

        (*rq_ptr).root_thread =  Some(tmp);//Arc::try_new(thread)?
        (*(*rq_ptr).root_thread.as_mut().unwrap().locked_data().get()).init()?;
        let pinned = Pin::new_unchecked(&mut *(Arc::into_raw( (*rq_ptr).root_thread.clone().unwrap()) as *mut SpinLock<rros_thread>));
        // &mut *Arc::into_raw( *(*rq_ptr).root_thread.clone().as_mut().unwrap()) as &mut SpinLock<rros_thread>
        spinlock_init!(pinned, "rros_threads");
        // (*rq_ptr).root_thread.as_mut().unwrap().assume_init();
    }
    Ok(0)
}

fn init_rtimer(rq_ptr: *mut rros_rq) -> Result<usize> {
    unsafe {
        let mut r = SpinLock::new(rros_timer::new(1));
        let pinned_r =  Pin::new_unchecked(&mut r);
        spinlock_init!(pinned_r, "rtimer");
        (*rq_ptr).root_thread.as_ref().unwrap().lock().rtimer = Some(Arc::try_new(r)?);
    }
    Ok(0)
}

fn init_ptimer(rq_ptr: *mut rros_rq) -> Result<usize> {
    unsafe {
        let mut p = SpinLock::new(rros_timer::new(1));
        let pinned_p =  Pin::new_unchecked(&mut p);
        spinlock_init!(pinned_p, "ptimer");
        (*rq_ptr).root_thread.as_ref().unwrap().lock().ptimer = Some(Arc::try_new(p)?);
    }
    Ok(0)
}

fn init_rq_ptr(rq_ptr: *mut rros_rq)-> Result<usize>  {
    unsafe {
            init_inband_timer(rq_ptr)?;
            init_rrbtimer(rq_ptr)?;
            init_root_thread(rq_ptr)?;
            init_rtimer(rq_ptr)?;
            init_ptimer(rq_ptr)?;
                // pr_info!("{:p}\n", &(*rq_ptr).local_flags);
                // (*rq_ptr) = rros_rq::new()?;
                // let mut x = SpinLock::new(RrosTimer::new(1));
                // let pinned =  Pin::new_unchecked(&mut x);
                // spinlock_init!(pinned, "inband_timer");
                // (*rq_ptr).inband_timer =  Some(Arc::try_new(x)?);
        
                // let mut y = SpinLock::new(RrosTimer::new(1));
                // let pinned = Pin::new_unchecked(&mut y);
                // spinlock_init!(pinned, "rrb_timer");
                // (*rq_ptr).rrbtimer =  Some(Arc::try_new(y)?);
        
                // let mut y = SpinLock::new(RrosTimer::new(1));
                // let pinned = Pin::new_unchecked(&mut y);
                // spinlock_init!(pinned, "rrb_timer");
                // (*rq_ptr).rrbtimer =  Some(Arc::try_new(y)?);
        
                // let pinned = Pin::new_unchecked(&mut (*rq_ptr).root_thread.unwrap());
                // spinlock_init!(pinned, "root_thread");
                
            // let mut thread = SpinLock::new(rros_thread::new()?);
            // let pinned = Pin::new_unchecked(&mut thread);
            // spinlock_init!(pinned, "rros_threads");
            // (*rq_ptr).root_thread =  Some(Arc::try_new(thread)?);
            
            // let mut r = SpinLock::new(rros_timer::new(1));
            // let pinned_r =  Pin::new_unchecked(&mut r);
            // spinlock_init!(pinned_r, "rtimer");
            // (*rq_ptr).root_thread.as_ref().unwrap().lock().rtimer = Some(Arc::try_new(r)?);

            // let mut p = SpinLock::new(rros_timer::new(1));
            // let pinned_p =  Pin::new_unchecked(&mut p);
            // spinlock_init!(pinned_p, "ptimer");
            // (*rq_ptr).root_thread.as_ref().unwrap().lock().ptimer = Some(Arc::try_new(p)?);
    }
    Ok(0)
}


fn init_rq_ptr_inband_timer(rq_ptr: *mut rros_rq)-> Result<usize>  {
    unsafe {
        let mut tmp = Arc::<SpinLock<rros_thread>>::try_new_uninit()?;
        let mut tmp = unsafe{
            core::ptr::write_bytes(Arc::get_mut_unchecked(&mut tmp), 0, 1);
            tmp.assume_init()
        };
        let pinned = unsafe{
           Pin::new_unchecked(Arc::get_mut_unchecked(&mut tmp))
        };
        spinlock_init!(pinned,"rros_kthreads");
        // let mut thread = SpinLock::new(rros_thread::new()?);
        // let pinned = Pin::new_unchecked(&mut thread);
        // spinlock_init!(pinned, "rros_threads");
        // Arc::get_mut(&mut tmp).unwrap().write(thread);

        (*rq_ptr).fifo.runnable.head =  Some(List::new(tmp));//Arc::try_new(thread)?
        (*(*rq_ptr).fifo.runnable.head.as_mut().unwrap().head.value.locked_data().get()).init()?;
        let pinned = Pin::new_unchecked(&mut *(Arc::into_raw( (*rq_ptr).fifo.runnable.head.as_mut().unwrap().head.value.clone()) as *mut SpinLock<rros_thread>));
        // &mut *Arc::into_raw( *(*rq_ptr).root_thread.clone().as_mut().unwrap()) as &mut SpinLock<rros_thread>
        spinlock_init!(pinned, "rros_threads");

        // let mut x = SpinLock::new(rros_thread::new()?);

        // let pinned = Pin::new_unchecked(&mut x);
        // spinlock_init!(pinned, "rros_runnable_thread");
        // (*rq_ptr).fifo.runnable.head = Some(List::new(Arc::try_new(x)?));
        // unsafe{(*rq_ptr).fifo.runnable.head = Some(List::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?));}
    }
    Ok(0)
}

pub struct rros_sched_attrs {
	pub sched_policy:i32,
	pub sched_priority:i32,
	// union {
	// 	struct __rros_rr_param rr;
	// 	struct __rros_quota_param quota;
	// 	struct __rros_tp_param tp;
	// } sched_u;
    pub tp_partition:i32,
}
impl rros_sched_attrs{
    pub fn new() -> Self{
        rros_sched_attrs{
            sched_policy:0,
            sched_priority:0,
            tp_partition:0,
        }
    }
}

use kernel::prelude::*;
use kernel::cpumask::{online_cpus, possible_cpus};

pub fn rros_init_sched() -> Result<usize> {
    unsafe {
        rros_runqueues = alloc_per_cpu(
            size_of::<rros_rq>() as usize,
            align_of::<rros_rq>() as usize,
        ) as *mut rros_rq;
        if rros_runqueues == 0 as *mut rros_rq {
            return Err(kernel::Error::ENOMEM);
        }
    }

    for cpu in possible_cpus() {
        pr_info!("{}\n", cpu);

        // let mut rq_ptr = this_rros_rq();
        let mut rq_ptr = unsafe{kernel::percpu_defs::per_cpu(rros_runqueues, cpu as i32)};
        unsafe {
            init_rq_ptr(rq_ptr)?;
            // // pr_info!("{:p}\n", &(*rq_ptr).local_flags);
            // (*rq_ptr) = rros_rq::new()?;
            // let mut x = SpinLock::new(RrosTimer::new(1));
            // let pinned = Pin::new_unchecked(&mut x);
            // spinlock_init!(pinned, "inband_timer");
            // (*rq_ptr).inband_timer = Some(Arc::try_new(x)?);

            // let mut y = SpinLock::new(RrosTimer::new(1));
            // let pinned = Pin::new_unchecked(&mut y);
            // spinlock_init!(pinned, "rrb_timer");
            // (*rq_ptr).rrbtimer = Some(Arc::try_new(y)?);

            // // let mut y = SpinLock::new(RrosTimer::new(1));
            // // let pinned = Pin::new_unchecked(&mut y);
            // // spinlock_init!(pinned, "rrb_timer");
            // // (*rq_ptr).rrbtimer =  Some(Arc::try_new(y)?);

            // // let pinned = Pin::new_unchecked(&mut (*rq_ptr).root_thread.unwrap());
            // // spinlock_init!(pinned, "root_thread");

            // let mut thread = SpinLock::new(rros_thread::new()?);
            // let pinned = Pin::new_unchecked(&mut thread);
            // spinlock_init!(pinned, "rros_threads");
            // (*rq_ptr).root_thread = Some(Arc::try_new(thread)?);

            // let mut r = SpinLock::new(rros_timer::new(1));
            // let pinned_r = Pin::new_unchecked(&mut r);
            // spinlock_init!(pinned_r, "rtimer");

            // let mut p = SpinLock::new(rros_timer::new(1));
            // let pinned_p = Pin::new_unchecked(&mut p);
            // spinlock_init!(pinned_p, "ptimer");

            // (*rq_ptr).root_thread.as_ref().unwrap().lock().rtimer = Some(Arc::try_new(r)?);
            // (*rq_ptr).root_thread.as_ref().unwrap().lock().ptimer = Some(Arc::try_new(p)?);

            // pr_info!("yinyongcishu is {}", Arc::strong_count(&(*rq_ptr).root_thread.clone().unwrap()));
        }
        // pr_info!("yinyongcishu is {}", Arc::strong_count(&(*rq_ptr).root_thread.clone().unwrap()));

        init_rq_ptr_inband_timer(rq_ptr)?;
        // unsafe {
        //     let mut x = SpinLock::new(rros_thread::new()?);

        //     let pinned = Pin::new_unchecked(&mut x);
        //     spinlock_init!(pinned, "rros_runnable_thread");
        //     (*rq_ptr).fifo.runnable.head = Some(List::new(Arc::try_new(x)?));
        //     // unsafe{(*rq_ptr).fifo.runnable.head = Some(List::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?));}
        // }
    }

    let ret = 0;
    // let cpu = 0;
    let ret = register_classes();
    match ret {
        Ok(_) => pr_info!("register_classes success!"),
        Err(e) => {
            pr_warn!("register_classes error!");
            return Err(e);
        }
    }

    // for_each_online_cpu()
    #[cfg(CONFIG_SMP)]
    for cpu in online_cpus(){
        unsafe {
            if rros_sched_topmost == 0 as *mut rros_sched_class {
                pr_info!("rros_sched_topmost is 0 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
            }
        }
        let mut rq_ptr = unsafe{kernel::percpu_defs::per_cpu(rros_runqueues, cpu as i32)};
        // TODO: fix the i32 problem
        let ret = unsafe { init_rq(rq_ptr, cpu as i32) };
        match ret {
            Ok(_) => pr_info!("init_rq success!"),
            Err(e) => {
                pr_warn!("init_rq error!");
                return Err(e);
            }
        }
    }
    // ret = bindings::__request_percpu_irq();
    pr_info!("sched init success!");
    Ok(0)
}

/* oob stalled. */
// #[cfg(CONFIG_SMP)]
// unsafe extern "C" fn oob_reschedule_interrupt(irq: i32 , dev_id:*mut c_types::c_void) -> bindings::irqreturn_t {
// 	// trace_rros_reschedule_ipi(this_rros_rq());

// 	/* Will reschedule from rros_exit_irq(). */

// 	return bindings::IRQ_HANDLED;
// }

// #[cfg(not(CONFIG_SMP))]
// unsafe extern "C" fn oob_reschedule_interrupt(irq: i32 , dev_id:*mut c_types::c_void) -> bindings::irqreturn_t {
// 	// trace_rros_reschedule_ipi(this_rros_rq());

// 	/* Will reschedule from rros_exit_irq(). */

// 	NULL;
// }

fn register_classes() -> Result<usize> {
    // let rros_sched_idle = unsafe{idle::rros_sched_idle};
    // let rros_sched_fifo = unsafe{fifo::rros_sched_fifo};
    let res = unsafe { register_one_class(&mut idle::rros_sched_idle, 1) };
    pr_info!(
        "after one register_one_class,topmost = {:p}",
        rros_sched_topmost
    );
    match res {
        Ok(_) => pr_info!("register_one_class(idle) success!"),
        Err(e) => {
            pr_warn!("register_one_class(idle) error!");
            return Err(e);
        }
    }
    // register_one_class(&mut rros_sched_weak);

    let res = unsafe{register_one_class(&mut tp::rros_sched_tp, 2,)};
	pr_info!("after two register_one_class,topmost = {:p}",rros_sched_topmost);
    match res{
		Ok(_) => pr_info!("register_one_class(tp) success!"),
		Err(e) =>{
			pr_warn!("register_one_class(tp) error!");
			return Err(e);
		},
	}
    let res = unsafe{register_one_class(&mut fifo::rros_sched_fifo, 3,)};
	pr_info!("after three register_one_class,topmost = {:p}",rros_sched_topmost);
    match res{
		Ok(_) => pr_info!("register_one_class(fifo) success!"),
		Err(e) =>{
			pr_warn!("register_one_class(fifo) error!");
			return Err(e);
		},
	}
    Ok(0)
}

// todo等全局变量实现后，去掉index和topmost
fn register_one_class(sched_class: &mut rros_sched_class, index: i32) -> Result<usize> {
    // let mut sched_class_lock = sched_class.lock();
    // let index = sched_class_lock.flag;
    // sched_class_lock.next = Some(rros_sched_topmost);
    // unsafe{sched_class.unlock()};
    unsafe{sched_class.next = rros_sched_topmost};
    if index ==1 {
        unsafe{rros_sched_topmost = &mut idle::rros_sched_idle as *mut rros_sched_class};
    } else if index ==2 {
        unsafe{rros_sched_topmost = &mut tp::rros_sched_tp as *mut rros_sched_class}; // FIXME: tp取消注释
        // unsafe{rros_sched_topmost  = 0 as *mut rros_sched_class};
    } else if index ==3 {
        unsafe{rros_sched_topmost = &mut fifo::rros_sched_fifo as *mut rros_sched_class};
    }
    pr_info!("in register_one_class,rros_sched_topmost = {:p}",rros_sched_topmost);
    if index != 3 {
        if index == 1 {
            unsafe{rros_sched_lower = &mut idle::rros_sched_idle as *mut rros_sched_class};
        }
        if index == 2 {
            unsafe{rros_sched_lower = &mut tp::rros_sched_tp as *mut rros_sched_class};// FIXME: tp实现后取消注释
        }
    }
    Ok(0)
}

// todo等全局变量实现后，去掉topmost
fn init_rq(rq: *mut rros_rq, cpu: i32) -> Result<usize> {
    let mut iattr = rros_init_thread_attr::new();
    let name_fmt: &'static CStr = c_str!("ROOT");
    // let mut rq_ptr = rq.borrow_mut();
    let mut rros_nrthreads = 0;
    unsafe {
        (*rq).proxy_timer_name = bindings::kstrdup(
            CStr::from_bytes_with_nul("[proxy-timer]\0".as_bytes())?.as_char_ptr(),
            bindings::GFP_KERNEL,
        )
    };
    unsafe {
        (*rq).rrb_timer_name = bindings::kstrdup(
            CStr::from_bytes_with_nul("[rrb-timer]\0".as_bytes())?.as_char_ptr(),
            bindings::GFP_KERNEL,
        )
    };

    let mut p = unsafe { rros_sched_topmost };

    pr_info!("before11111111111111111111111111111111111111");
    while p != 0 as *mut rros_sched_class {
        pr_info!("p!=0!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        if unsafe { (*p).sched_init != None } {
            let func;
            unsafe {
                match (*p).sched_init {
                    Some(f) => func = f,
                    None => {
                        pr_warn!("sched_init function error");
                        return Err(kernel::Error::EINVAL);
                    }
                }
            }
            func(rq);
        }
        unsafe { p = (*p).next };
    }
    pr_info!("after11111111111111111111111111111111111111");

    unsafe { (*rq).flags = 0 };
    unsafe { (*rq).local_flags = RQ_IDLE };
    // pr_info!("yinyongcishu is {}", Arc::strong_count(&(*rq).root_thread.clone().unwrap()));
    let a = unsafe { (*rq).root_thread.clone() };
    if a.is_some() {
        unsafe { (*rq).curr = a.clone() };
    }
    // pr_info!("The state is {:}\n", (*rq).get_curr().lock().state);
    unsafe {
        rros_init_timer_on_rq(
            (*rq).get_inband_timer(),
            unsafe { &mut clock::RROS_MONO_CLOCK },
            None,
            rq,
            c_str!("tick"),
            RROS_TIMER_IGRAVITY,
        )
    };
    unsafe {
        rros_init_timer_on_rq(
            (*rq).get_rrbtimer(),
            unsafe { &mut clock::RROS_MONO_CLOCK },
            Some(roundrobin_handler),
            rq,
            c_str!("rrb"),
            RROS_TIMER_IGRAVITY,
        )
    };
    // rros_set_timer_name(&rq->inband_timer, rq->proxy_timer_name);
    // rros_init_timer_on_rq(&rq->rrbtimer, &rros_mono_clock, roundrobin_handler,
    // 		rq, RROS_TIMER_IGRAVITY);
    // rros_set_timer_name(&rq->rrbtimer, rq->rrb_timer_name);

    // rros_set_current_account(rq, &rq->root_thread.stat.account);
    iattr.flags = T_ROOT as i32;
    iattr.affinity = cpumask_of(cpu);
    // todo等全局变量
    unsafe {
        iattr.sched_class = Some(&idle::rros_sched_idle);
    }
    // 下面多数注释的都是由于rros_init_thread未完成导致的
    // let sched_param_clone;
    // let mut sched_param_ptr;
    // match iattr.sched_param {
    //     Some(p) => sched_param_clone = p.clone(),
    //     None => return Err(kernel::Error::EINVAL),
    // }
    // sched_param_ptr = sched_param_clone.borrow_mut();
    // sched_param_ptr.idle.prio = idle::RROS_IDLE_PRIO;

    let sched_param =  unsafe{Arc::try_new(SpinLock::new(rros_sched_param::new()))?};
    unsafe{(*sched_param.locked_data().get()).fifo.prio = idle::RROS_IDLE_PRIO};
    iattr.sched_param = Some(sched_param);

    // pr_info!("yinyongcishu is {}", Arc::strong_count(&(*rq).root_thread.clone().unwrap()));
    // pr_info!("yinyongcishu is {}", Arc::strong_count(&(*rq).root_thread.clone().unwrap()));
    unsafe { rros_init_thread(&(*rq).root_thread.clone(), iattr, rq, name_fmt) }; //c_str!("0").as_char_ptr()

    unsafe {
        let next_add = (*rq).root_thread.clone().unwrap().lock().deref_mut() as *mut rros_thread;
        // pr_info!("the root thread add is  next_add {:p}", next_add);
    }

    // pr_info!("The state is {:}\n", (*rq).get_curr().lock().state);
    let rq_root_thread_2;
    unsafe {
        match (*rq).root_thread.clone() {
            Some(rt) => rq_root_thread_2 = rt.clone(),
            None => {
                pr_warn!("use rq.root_thread error");
                return Err(kernel::Error::EINVAL);
            }
        }
    }
    // let mut rq_root_thread_lock = rq_root_thread_2.lock();
    let add = &mut rq_root_thread_2.lock().deref_mut().altsched
        as *mut bindings::dovetail_altsched_context;
    unsafe { bindings::dovetail_init_altsched(add) };

    // let mut rros_thread_list = list_head::new();
    // list_add_tail(
    //     &mut rq_root_thread_lock.next as *mut list_head,
    //     &mut rros_thread_list as *mut list_head,
    // );
    // rros_nrthreads += 1;
    Ok(0)
}

fn rros_sched_tick(rq: *mut rros_rq) {
    let curr;
    unsafe {
        curr = (*rq).get_curr();
    }
    let sched_class = curr.lock().sched_class.clone().unwrap();
    let flags = curr.lock().base_class.clone().unwrap().flag;
    let state = curr.lock().state;
    let a = sched_class.flag == flags;
    let b = !sched_class.sched_tick.is_none();
    let c = state & (RROS_THREAD_BLOCK_BITS | T_RRB) == T_RRB;
    let d = rros_preempt_count() == 0;
    // pr_info!("The current root state {}", (*rq).root_thread.as_ref().unwrap().lock().state);
    pr_info!("abcd {} {} {} {} state{} 2208\n", a, b, c, d, state);
    if a && b && c && d {
        unsafe {
            sched_class.sched_tick.unwrap()(Some(rq));
        }
        // sched_class->sched_tick(rq);
    }
}

pub fn roundrobin_handler(timer: *mut RrosTimer) {
    let rq = this_rros_rq();
    rros_sched_tick(rq);
}

pub fn cpumask_of(cpu: i32) -> *const cpumask::CpumaskT {
    return unsafe { rust_helper_cpumask_of(cpu) };
}

fn list_add_tail(new: *mut list_head, head: *mut list_head) {
    unsafe { rust_helper_list_add_tail(new, head) };
}

pub fn rros_set_effective_thread_priority(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    prio: i32,
) -> Result<usize> {
    let thread_clone = thread.clone();
    let thread_unwrap = thread_clone.unwrap();
    let base_class;
    match thread_unwrap.lock().base_class.clone() {
        Some(t) => base_class = t,
        None => return Err(kernel::Error::EINVAL),
    };
    let wprio: i32 = rros_calc_weighted_prio(base_class, prio);
    thread_unwrap.lock().bprio = prio;

    let thread_wprio = thread_unwrap.lock().wprio;
    let state = thread_unwrap.lock().state;
    if (wprio == thread_wprio) {
        return Ok(0);
    }

    if (wprio < thread_wprio && (state & T_BOOST) != 0) {
        return Err(kernel::Error::EINVAL);
    }

    thread_unwrap.lock().cprio = prio;

    Ok(0)
}

pub fn rros_track_priority(thread:Arc<SpinLock<rros_thread>>,p:Arc<SpinLock<rros_sched_param>>) -> Result<usize>{
    unsafe{
        let func;
        match (*thread.locked_data().get()).sched_class.unwrap().sched_trackprio{
            Some(f) => func = f,
            None => {
                pr_warn!("rros_get_schedparam: sched_trackprio function error");
                return Err(kernel::Error::EINVAL);
            }
        };
        func(Some(thread.clone()), Some(p.clone()));

        let sched_class = (*thread.locked_data().get()).sched_class.unwrap();
        let prio = (*thread.locked_data().get()).cprio;
        (*thread.locked_data().get()).wprio = rros_calc_weighted_prio(sched_class,  prio);
    }
    Ok(0)
}

fn rros_ceil_priority(thread:Arc<SpinLock<rros_thread>>, prio:i32) -> Result<usize>{
	unsafe{
        let func;
        match (*thread.locked_data().get()).sched_class.unwrap().sched_ceilprio{
            Some(f) => func = f,
            None => {
                pr_warn!("rros_ceil_priority:sched_ceilprio function error");
                return Err(kernel::Error::EINVAL);
            }
        }
        func(thread.clone(), prio);
        let sched_class = (*thread.locked_data().get()).sched_class.unwrap();
        let prio = (*thread.locked_data().get()).cprio;
        (*thread.locked_data().get()).wprio = rros_calc_weighted_prio(sched_class, prio);
    }
    Ok(0)
}

pub fn rros_calc_weighted_prio(sched_class: &'static rros_sched_class, prio: i32) -> i32 {
    return prio + sched_class.weight;
}

pub fn rros_putback_thread(thread: Arc<SpinLock<rros_thread>>) -> Result<usize> {
    let mut state = thread.lock().state;
    if state & T_READY != 0 {
        rros_dequeue_thread(thread.clone());
    } else {
        thread.lock().state |= T_READY;
    }
    rros_enqueue_thread(thread.clone());
    let rq = thread.lock().rq;
    rros_set_resched(rq);
    Ok(0)
}

//未测试，应该可行
pub fn rros_dequeue_thread(thread: Arc<SpinLock<rros_thread>>) -> Result<usize> {
    let sched_class;
    match thread.lock().sched_class.clone() {
        Some(c) => sched_class = c,
        None => return Err(kernel::Error::EINVAL),
    }
    if sched_class.flag == 3 {
        fifo::__rros_dequeue_fifo_thread(thread.clone());
    } else if sched_class.flag != 1 {
        let func;
        match sched_class.sched_dequeue {
            Some(f) => func = f,
            None => return Err(kernel::Error::EINVAL),
        }
        func(thread.clone());
    }
    Ok(0)
}

//未测试，应该可行
pub fn rros_enqueue_thread(thread: Arc<SpinLock<rros_thread>>) -> Result<usize> {
    let sched_class;
    match thread.lock().sched_class.clone() {
        Some(c) => sched_class = c,
        None => return Err(kernel::Error::EINVAL),
    }
    if sched_class.flag == 3 {
        fifo::__rros_enqueue_fifo_thread(thread.clone());
    } else if sched_class.flag != 1 {
        let func;
        match sched_class.sched_enqueue {
            Some(f) => func = f,
            None => return Err(kernel::Error::EINVAL),
        }
        func(thread.clone());
    }
    Ok(0)
}

//未测试，应该可行
pub fn rros_requeue_thread(thread: Arc<SpinLock<rros_thread>>) -> Result<usize> {
    let sched_class;
    unsafe {
        match (*thread.locked_data().get()).sched_class.clone() {
            Some(c) => sched_class = c,
            None => return Err(kernel::Error::EINVAL),
        }
    }
    if sched_class.flag == 3 {
        fifo::__rros_requeue_fifo_thread(thread.clone());
    } else if sched_class.flag != 1 {
        let func;
        match sched_class.sched_requeue {
            Some(f) => func = f,
            None => return Err(kernel::Error::EINVAL),
        }
        func(thread.clone());
    }
    Ok(0)
}

// fn rros_need_resched(rq: *mut rros_rq) -> bool {
//     unsafe{(*rq).flags & RQ_SCHED != 0x0}
// }
// static inline int rros_need_resched(struct rros_rq *rq)
// {
// 	return rq->flags & RQ_SCHED;
// }

/* hard irqs off. */
fn test_resched(rq: *mut rros_rq) -> bool {
    let need_resched = rros_need_resched(rq);
    // #ifdef CONFIG_SMP
    /* Send resched IPI to remote CPU(s). */
    // if (unlikely(!cpumask_empty(&this_rq->resched_cpus))) {
    // 	irq_send_oob_ipi(RESCHEDULE_OOB_IPI, &this_rq->resched_cpus);
    // 	cpumask_clear(&this_rq->resched_cpus);
    // 	this_rq->local_flags &= ~RQ_SCHED;
    // }

    if need_resched {
        unsafe { (*rq).flags &= !RQ_SCHED }
        // unsafe{(*rq).local_flags &= !RQ_SCHED}
    }

    need_resched
}
// static __always_inline bool test_resched(struct rros_rq *this_rq)
// {
// 	bool need_resched = rros_need_resched(this_rq);

// #ifdef CONFIG_SMP
// 	/* Send resched IPI to remote CPU(s). */
// 	if (unlikely(!cpumask_empty(&this_rq->resched_cpus))) {
// 		irq_send_oob_ipi(RESCHEDULE_OOB_IPI, &this_rq->resched_cpus);
// 		cpumask_clear(&this_rq->resched_cpus);
// 		this_rq->local_flags &= ~RQ_SCHED;
// 	}
// #endif
// 	if (need_resched)
// 		this_rq->flags &= ~RQ_SCHED;

// 	return need_resched;
// }

//逻辑完整，未测试
#[no_mangle]
pub unsafe extern "C" fn rros_schedule() {
    unsafe {
        if rros_runqueues == 0 as *mut rros_rq {
            return;
        }
    }

    let this_rq = this_rros_rq();
    let flags;
    let local_flags;

    unsafe {
        flags = (*this_rq).flags;
        local_flags = (*this_rq).local_flags;
    }

    // pr_info!(
    //     "rros_schedule: flags is {} local_flags is {}\n",
    //     flags,
    //     local_flags
    // );

    //b kernel/rros/sched.rs:1670
    if ((flags | local_flags) & (RQ_IRQ | RQ_SCHED)) != RQ_SCHED {
        return;
    }

    let res = premmpt::running_inband();
    let r = match res {
        Ok(_o) => true,
        Err(_e) => false,
    };
    if !r {
        unsafe {
            __rros_schedule(0 as *mut c_types::c_void);
        }
        return;
    }

    unsafe {
        bindings::run_oob_call(Some(__rros_schedule), 0 as *mut c_types::c_void);
    }
}

extern "C" {
    fn rust_helper_hard_local_irq_save() -> c_types::c_ulong;
    fn rust_helper_hard_local_irq_restore(flags: c_types::c_ulong);
    fn rust_helper_preempt_enable();
    fn rust_helper_preempt_disable();
}

#[no_mangle]
unsafe extern "C" fn __rros_schedule(arg: *mut c_types::c_void) -> i32 {
    unsafe {
        // fn __rros_schedule() {
        // pr_info!("sched thread!!!!");
        // let prev = curr;
        let prev;
        let curr;
        let next;
        let this_rq = this_rros_rq();
        let mut leaving_inband;

        let flags = unsafe { rust_helper_hard_local_irq_save() };

        unsafe {
            curr = (*this_rq).get_curr();
        }

        let curr_state = unsafe { (*curr.locked_data().get()).state };
        if curr_state & T_USER != 0x0 {
            //rros_commit_monitor_ceiling();
        }

        //这里可以不用自旋锁，因为只有一个cpu，理论上来说没有问题
        // raw_spin_lock(&curr->lock);
        // raw_spin_lock(&this_rq->lock);

        if !(test_resched(this_rq)) {
            // raw_spin_unlock(&this_rq->lock);
            // raw_spin_unlock_irqrestore(&curr->lock, flags);
            // rust_helper_hard_local_irq_restore(flags);
            lock::raw_spin_unlock_irqrestore(flags);
            return 0;
        }

        let curr_add = curr.locked_data().get();
        next = pick_next_thread(Some(this_rq)).unwrap();
        // unsafe{pr_info!("begin of the rros_schedule uninit_thread: x ref is {}", Arc::strong_count(&next.clone()));}

        let next_add = next.locked_data().get();

        if next_add == curr_add {
            // if the curr and next are both root, we should call the inband thread
            pr_info!("__rros_schedule: next_add == curr_add ");
            let next_state = unsafe { (*next.locked_data().get()).state };
            if (next_state & T_ROOT as u32) != 0x0 {
                if unsafe { (*this_rq).local_flags & RQ_TPROXY != 0x0 } {
                    pr_info!("__rros_schedule: (*this_rq).local_flags & RQ_TPROXY != 0x0 ");
                    tick::rros_notify_proxy_tick(this_rq);
                }
                if unsafe { (*this_rq).local_flags & RQ_TDEFER != 0x0 } {
                    pr_info!("__rros_schedule: (*this_rq).local_flags & RQ_TDEFER !=0x0 ");
                    unsafe {
                        tick::rros_program_local_tick(
                            &mut clock::RROS_MONO_CLOCK as *mut clock::RrosClock,
                        );
                    }
                }
            }
            // rust_helper_hard_local_irq_restore(flags);
            lock::raw_spin_unlock_irqrestore(flags);
            return 0;
        }

        prev = curr.clone();
        unsafe {
            (*this_rq).curr = Some(next.clone());
        }
        // unsafe{pr_info!("mid of the rros_schedule uninit_thread: x ref is {}", Arc::strong_count(&next.clone()));}
        leaving_inband = false;

        let prev_state = (*prev.locked_data().get()).state;
        let next_state = (*next.locked_data().get()).state;
        if prev_state & T_ROOT as u32 != 0x0 {
            // leave_inband(prev);
            leaving_inband = true;
        } else if (next_state & T_ROOT as u32 != 0x0) {
            if unsafe { (*this_rq).local_flags & RQ_TPROXY != 0x0 } {
                tick::rros_notify_proxy_tick(this_rq);
            }
            if unsafe { (*this_rq).local_flags & RQ_TDEFER != 0x0 } {
                unsafe {
                    tick::rros_program_local_tick(
                        &mut clock::RROS_MONO_CLOCK as *mut clock::RrosClock,
                    );
                }
            }
            // enter_inband(next);
        }

        // prepare_rq_switch(this_rq, prev, next);

        let prev_add = prev.locked_data().get();
        // pr_info!("the run thread add is  spinlock prev {:p}", prev_add);

        let next_add = next.locked_data().get();
        // pr_info!("the run thread add is  spinlock  next {:p}", next_add);
        // pr_info!("the run thread add is  arc prev {:p}", prev);
        // pr_info!("the run thread add is  arc next {:p}", next);

        // fix!!!!!
        let prev_sched =
            &mut (*prev.locked_data().get()).altsched as *mut bindings::dovetail_altsched_context;
        let next_sched =
            &mut (*next.locked_data().get()).altsched as *mut bindings::dovetail_altsched_context;
        let inband_tail;
        // pr_info!("before the inband_tail next state is {}", next.lock().state);
        unsafe {
            inband_tail = bindings::dovetail_context_switch(prev_sched, next_sched, leaving_inband);
        }
        // next.unlock();
        // finish_rq_switch(inband_tail, flags); //b kernel/rros/sched.rs:1751

        // if prev ==
        // bindings::dovetail_context_switch();
        // inband_tail = dovetail_context_switch(&prev->altsched,
        //     &next->altsched, leaving_inband);

        // rust_helper_hard_local_irq_restore(flags);
        // pr_info!("before the inband_tail curr state is {}", curr.lock().state);

        pr_info!("the inband_tail is {}", inband_tail);
        if inband_tail == false {
            lock::raw_spin_unlock_irqrestore(flags);
        }
        unsafe{pr_info!("end of the rros_schedule uninit_thread: x ref is {}", Arc::strong_count(&next.clone()));}
        0
    }
}

// TODO: add this function
fn finish_rq_switch() {}

pub fn pick_next_thread(rq: Option<*mut rros_rq>) -> Option<Arc<SpinLock<rros_thread>>> {
    let mut oob_mm = &mut bindings::oob_mm_state::default() as *mut bindings::oob_mm_state;
    let mut next: Option<Arc<SpinLock<rros_thread>>> = None;
    loop {
        next = __pick_next_thread(rq);
        let next_clone = next.clone().unwrap();
        oob_mm = unsafe { (*next_clone.locked_data().get()).oob_mm };
        if oob_mm.is_null() {
            break;
        }
        unsafe {
            if test_bit(RROS_MM_PTSYNC_BIT, &(*oob_mm).flags as *const u32) == false {
                break;
            }
        }
        let info = unsafe { (*next_clone.locked_data().get()).info };
        if info & (T_PTSTOP | T_PTSIG | T_KICKED) != 0 {
            break;
        }
        unsafe { (*next_clone.locked_data().get()).state |= T_PTSYNC };
        unsafe { (*next_clone.locked_data().get()).state &= !T_READY };
    }
    set_next_running(rq.clone(), next.clone());

    return next;
}

//逻辑不完整，但应该没问题，未测试
pub fn __pick_next_thread(rq: Option<*mut rros_rq>) -> Option<Arc<SpinLock<rros_thread>>> {
    let curr = unsafe { (*rq.clone().unwrap()).curr.clone().unwrap() };

    let mut next: Option<Arc<SpinLock<rros_thread>>> = None;

    let mut state = unsafe { (*curr.locked_data().get()).state };
    if state & (RROS_THREAD_BLOCK_BITS | T_ZOMBIE) == 0 {
        if rros_preempt_count() > 0 {
            rros_set_self_resched(rq);
            return Some(curr.clone());
        }
        if state & T_READY == 0 {
            rros_requeue_thread(curr.clone());
            state |= T_READY;
        }
    }

    next = lookup_fifo_class(rq.clone());
    // pr_info!("next2");
    if next.is_some() {
        pr_info!("__pick_next_thread: next.is_some");
        return next;
    }

    //这里虽然没有循环，但应该没有问题
    //TODO: 这里的for循环
    let mut next;
    let mut p = unsafe { rros_sched_lower };
    while p != 0 as *mut rros_sched_class {
        pr_info!("p!=0!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! in sched_pick");
        if unsafe { (*p).sched_pick != None } {
            let func;
            unsafe {
                match (*p).sched_pick {
                    Some(f) => func = f,
                    None => {
                        pr_warn!("sched_pick function error, this should not happen");
                        return None;
                        // return Err(kernel::Error::EINVAL);
                    }
                }
            }
            next = func(rq.clone());
            match next {
                Ok(n) => return Some(n),
                Err(e) => {
                    pr_warn!("nothing found");
                },
            }
        }
        unsafe { p = (*p).next };
    }
    // let func = unsafe { idle::rros_sched_idle.sched_pick.unwrap() };
    // let next = func(rq.clone());
    // match next {
    //     Ok(n) => return Some(n),
    //     Err(e) => return None,
    // }
    return None;
}

//逻辑完整，未测试
pub fn lookup_fifo_class(rq: Option<*mut rros_rq>) -> Option<Arc<SpinLock<rros_thread>>> {
    let q = &mut unsafe { (*rq.clone().unwrap()).fifo.runnable.head.as_mut().unwrap() };
    if q.is_empty() {
        return None;
    }
    // pr_info!("next0");
    let thread = q.get_head().unwrap().value.clone();
    let sched_class = unsafe { (*thread.locked_data().get()).sched_class.clone().unwrap() };

    if sched_class.flag != 3 {
        let func = sched_class.sched_pick.unwrap();
        return Some(func(rq).unwrap());
    }

    pr_info!("lookup_fifo_class :2");
    q.de_head();
    return Some(thread.clone());
}

//逻辑完整，未测试
pub fn set_next_running(rq: Option<*mut rros_rq>, next: Option<Arc<SpinLock<rros_thread>>>) {
    let next = next.unwrap();
    unsafe { (*next.locked_data().get()).state &= !T_READY };
    let state = unsafe { (*next.locked_data().get()).state };
    pr_info!("set_next_running: next.lock().state is {}", unsafe {
        (*next.locked_data().get()).state
    });
    if state & T_RRB != 0 {
        unsafe {
            let delta = (*next.locked_data().get()).rrperiod;
            rros_start_timer(
                (*rq.clone().unwrap()).rrbtimer.clone().unwrap(),
                rros_abs_timeout((*rq.clone().unwrap()).rrbtimer.clone().unwrap(), delta),
                RROS_INFINITE,
            )
        };
    } else {
        unsafe { rros_stop_timer((*rq.clone().unwrap()).rrbtimer.clone().unwrap()) };
    }
}

fn rros_preempt_count() -> i32 {
    unsafe { return (*rust_helper_dovetail_current_state()).preempt_count };
}

fn test_bit(nr: i32, addr: *const u32) -> bool {
    unsafe { return rust_helper_test_bit(nr, addr) };
}

pub fn rros_set_thread_policy(thread: Option<Arc<SpinLock<rros_thread>>>,
    sched_class:Option<&'static rros_sched_class>,
    p:Option<Arc<SpinLock<rros_sched_param>>>) ->Result<usize>
{
    let mut flags:c_types::c_ulong = 0;
    // let test = p.clone().unwrap();
    let mut rq: Option<*mut rros_rq>;
    rq = rros_get_thread_rq(thread.clone(), &mut flags);
    pr_info!("rros_get_thread_rq success");
    rros_set_thread_policy_locked(thread.clone(), sched_class.clone(), p.clone())?;
    pr_info!("rros_set_thread_policy_locked success");
    rros_put_thread_rq(thread.clone(), rq.clone(), flags);
    pr_info!("rros_put_thread_rq success");
    Ok(0)
}

pub fn rros_get_thread_rq(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    flags: &mut c_types::c_ulong,
) -> Option<*mut rros_rq> {
    // pr_info!("yinyongcishu is {}", Arc::strong_count(&thread.clone().unwrap()));
    //todo raw_spin_lock_irqsave and raw_spin_lock
    *flags = unsafe { rust_helper_hard_local_irq_save() };
    // unsafe{rust_helper_preempt_disable();}
    unsafe { (*thread.unwrap().locked_data().get()).rq.clone() }
}

pub fn rros_put_thread_rq(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    rq: Option<*mut rros_rq>,
    flags: c_types::c_ulong,
) -> Result<usize> {
    unsafe {
        rust_helper_hard_local_irq_restore(flags);
        // rust_helper_preempt_enable();
    }
    //todo  raw_spin_unlock and raw_spin_unlock_irqrestore
    Ok(0)
}

pub fn rros_set_thread_policy_locked(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    sched_class:Option<&'static rros_sched_class>,
    p:Option<Arc<SpinLock<rros_sched_param>>>) ->Result<usize>
{
    let test = p.clone().unwrap();
    let thread_unwrap = thread.clone().unwrap();
    let orig_effective_class: Option<Rc<RefCell<rros_sched_class>>> = None;
    let mut effective: Result<usize>;
    rros_check_schedparams(thread.clone(), sched_class.clone(), p.clone())?;
    let mut flag_base_class = 0;
    let mut base_class = thread_unwrap.lock().base_class;
    if base_class.is_none() {
        // pr_info!("baseclass is none!");
        flag_base_class = 1;
    }

    unsafe {
        if flag_base_class == 1
            || (sched_class.unwrap() as *const rros_sched_class)
                != (base_class.unwrap() as *const rros_sched_class)
        {
            rros_declare_thread(thread.clone(), sched_class.clone(), p.clone());
        }
    }
    // pr_info!("yinyongcishu is {}", Arc::strong_count(&thread.clone().unwrap()));
    if base_class.is_some() {
        let state = thread_unwrap.lock().state;
        if state & T_READY != 0x0 {
            rros_dequeue_thread(thread.clone().unwrap());
        }

        if (sched_class.unwrap() as *const rros_sched_class)
            != (base_class.unwrap() as *const rros_sched_class)
        {
            rros_forget_thread(thread.clone().unwrap());
        }
    }
    thread_unwrap.lock().base_class = sched_class.clone();
    // todo RROS_DEBUG
    // if (RROS_DEBUG(CORE)) {
    //     orig_effective_class = thread->sched_class;
    //     thread->sched_class = NULL;
    // }
    // let test = p.clone().unwrap();
    // pr_info!("! yinyongcishu is {}", Arc::strong_count(&thread.clone().unwrap()));
    effective = rros_set_schedparam(thread.clone(), p.clone());
    // pr_info!("thread after setting {}", thread_unwrap.lock().state);
    // pr_info!("! yinyongcishu is {}", Arc::strong_count(&thread.clone().unwrap()));
    if effective == Ok(0) {
        thread_unwrap.lock().sched_class = sched_class.clone();
        let cprio = thread_unwrap.lock().cprio;
        let wprio = rros_calc_weighted_prio(sched_class.clone().unwrap(), cprio);
        thread_unwrap.lock().wprio = wprio;
    }
    // todo RROS_DEBUG
    // else if (RROS_DEBUG(CORE))
    //     thread->sched_class = orig_effective_class;
    let state = thread_unwrap.lock().state;
    if state & T_READY != 0x0 {
        // pr_info!("wwwwwwwwhat the fuck!");
        rros_enqueue_thread(thread.clone().unwrap());
    }

    let state = thread_unwrap.lock().state;
    if state & (T_DORMANT | T_ROOT as u32) == 0x0 {
        let rq = thread_unwrap.lock().rq;
        rros_set_resched(rq);
    }
    // pr_info!("hy kkkkk3 {}", thread_unwrap.lock().state);
    // pr_info!("yinyongcishu is {}", Arc::strong_count(&thread.clone().unwrap()));
    Ok(0)
}

//逻辑完整，未测试
fn rros_check_schedparams(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    sched_class:Option<&'static rros_sched_class>,
    p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<usize>
{

    let sched_class_ptr = sched_class.unwrap();
    if sched_class_ptr.sched_chkparam.is_some() {
        let func = sched_class_ptr.sched_chkparam.unwrap();
        func(thread.clone(), p.clone());
    } else {
        pr_info!("rros_check_schedparams no sched_chkparam functions");
    }
    Ok(0)
}

pub fn rros_get_schedparam(thread:Arc<SpinLock<rros_thread>>,p:Arc<SpinLock<rros_sched_param>>) ->Result<usize>{
    let func;
    unsafe{
        match (*thread.locked_data().get()).sched_class.unwrap().sched_getparam{
            Some(f) => func = f,
            None => {
                pr_warn!("rros_get_schedparam: sched_getparam function error");
                return Err(kernel::Error::EINVAL);
            }
        };
        func(Some(thread.clone()), Some(p.clone()));
    }
    Ok(0)
}

fn rros_set_schedparam(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<usize>
{
    let thread_clone = thread.clone();
    let thread_unwrap = thread_clone.unwrap();
    // let thread_lock = thread_unwrap.lock();
    let base_class_clone = thread_unwrap.lock().base_class.clone();
    if base_class_clone.is_none() {
        pr_info!("rros_set_schedparam: finded");
    }
    let base_class_unwrap = base_class_clone.unwrap();
    let func = base_class_unwrap.sched_setparam.unwrap();
    // pr_info!("thread before setting {}", thread_unwrap.lock().state);
    let res = func(thread.clone(), p.clone());
    // pr_info!("thread before calling {}", thread_unwrap.lock().state);
    res
    // return ;
}

//逻辑完整，未测试，待重构
fn rros_declare_thread(
    thread: Option<Arc<SpinLock<rros_thread>>>,
    sched_class: Option<&'static rros_sched_class>,
    p: Option<Arc<SpinLock<rros_sched_param>>>)->Result<usize>
{
    let thread_clone = thread.clone();
    let thread_unwrap = thread_clone.unwrap();
    let mut sched_class_ptr = sched_class.unwrap();
    if sched_class_ptr.sched_declare.is_some() {
        let func = sched_class_ptr.sched_declare.unwrap();
        func(thread.clone(), p.clone())?;
    }
    let base_class = thread_unwrap.lock().base_class;
    unsafe {
        if base_class.is_none()
            || (sched_class_ptr as *const rros_sched_class)
                != (base_class.unwrap() as *const rros_sched_class)
        {
            let mut sched_class_mutptr = sched_class_ptr as *const rros_sched_class;
            let mut sched_class_mutptr_mut = sched_class_mutptr as *mut rros_sched_class;
            (*sched_class_mutptr_mut).nthreads += 1;
        }
    }

    pr_info!("rros_declare_thread success!");
    Ok(0)
}

pub fn rros_forget_thread(thread: Arc<SpinLock<rros_thread>>) -> Result<usize> {
    let thread_clone = thread.clone();
    // let thread_lock = thread_clone.lock();
    let sched_class = thread_clone.lock().base_class.clone();
    let mut sched_class_ptr = sched_class.unwrap() as *const rros_sched_class;
    let mut sched_class_ptr = sched_class_ptr as *mut rros_sched_class;
    unsafe {
        (*sched_class_ptr).nthreads -= 1;
    }

    unsafe {
        if (*sched_class_ptr).sched_forget.is_some() {
            let func = (*sched_class_ptr).sched_forget.unwrap();
            func(thread.clone());
        }
    }

    Ok(0)
}

extern "C" {
    fn rust_helper_unstall_oob();
    fn rust_helper_stall_oob();
}

#[no_mangle]
unsafe extern "C" fn rust_resume_oob_task(ptr: *mut c_types::c_void) {
    // struct rros_thread *thread = rros_thread_from_task(p);

    // pr_info!("rros rros mutex ptr{:p}", ptr);
    let thread: Arc<SpinLock<rros_thread>>;

    unsafe {
        thread = Arc::from_raw(ptr as *mut SpinLock<rros_thread>);
        unsafe{pr_info!("0600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
        Arc::increment_strong_count(ptr);
        unsafe{pr_info!("b600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
        pr_info!("the ptr in resume address is {:p}", ptr);
        unsafe{pr_info!("a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
        // unsafe{pr_info!("600 uninit_thread: x ref is {}", Arc::strong_count(ptr));}
    }
    unsafe{pr_info!("2a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}

    /*
     * Dovetail calls us with hard irqs off, oob stage
     * stalled. Clear the stall bit which we don't use for
     * protection but keep hard irqs off.
     */
    unsafe {
        rust_helper_unstall_oob();
    }
    // check_cpu_affinity(p);
    unsafe{pr_info!("3a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
    rros_release_thread(thread.clone(), T_INBAND, 0);
    /*
     * If T_PTSTOP is set, pick_next_thread() is not allowed to
     * freeze @thread while in flight to the out-of-band stage.
     */
    unsafe{pr_info!("4a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
    unsafe {
        sched::rros_schedule();
        unsafe{pr_info!("5a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
        rust_helper_stall_oob();
    }
    unsafe{pr_info!("6a600 uninit_thread: x ref is {}", Arc::strong_count(&thread));}
}

extern "C" {
    fn rust_helper_hard_local_irq_disable();
    fn rust_helper_dovetail_leave_oob();
    fn rust_helper_hard_local_irq_enable();
}

//基本完整
pub fn rros_switch_inband(cause: i32) {
    pr_info!("rros_switch_inband: in");
    let curr = unsafe { &mut *rros_current() };
    let this_rq: Option<*mut rros_rq>;
    let notify: bool;
    unsafe {
        rust_helper_hard_local_irq_disable();
    }
    curr.lock().inband_work.irq_work_queue();
    this_rq = curr.lock().rq.clone();

    curr.lock().state |= T_INBAND;
    curr.lock().local_info &= !T_SYSRST;
    notify = curr.lock().state & T_USER != 0x0 && cause > (RROS_HMDIAG_NONE as i32);

    let info = curr.lock().info;
    if cause == (RROS_HMDIAG_TRAP as i32) {
        pr_info!("rros_switch_inband: cause == RROS_HMDIAG_TRAP");
        // TODO:
    } else if info & T_PTSIG != 0x0 {
        pr_info!("rros_switch_inband: curr->info & T_PTSIG");
        // TODO:
    }

    curr.lock().info &= !RROS_THREAD_INFO_MASK;
    rros_set_resched(this_rq);
    unsafe {
        rust_helper_dovetail_leave_oob();
        __rros_schedule(0 as *mut c_types::c_void);
        rust_helper_hard_local_irq_enable();
        bindings::dovetail_resume_inband();
    }

    // curr.lock().stat.isw.inc_counter();
    // rros_propagate_schedparam_change(curr);

    if notify {
        // TODO:
    }

    //rros_sync_uwindow(curr); todo
}

pub enum RrosValue{
    Val(i32),
    Lval(i64),
    Ptr(*mut c_types::c_void),
}

impl RrosValue{
    pub fn new() -> Self{
        RrosValue::Lval(0)
    }
    pub fn new_nil() -> Self{
        RrosValue::Ptr(null_mut())
    }
}

#[inline]
pub fn rros_enable_preempt(){
    extern "C"{
        fn rust_helper_rros_enable_preempt_top_part()->bool;
    }
    unsafe{
        if rust_helper_rros_enable_preempt_top_part(){
            rros_schedule()
        }  
    } 
}

#[inline]
pub fn rros_disable_preempt(){
    extern "C"{
        fn rust_helper_rros_disable_preempt();
    }
    unsafe{
        rust_helper_rros_disable_preempt();
    }
}