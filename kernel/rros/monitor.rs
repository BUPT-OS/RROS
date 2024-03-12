use core::{
    cell::RefCell,
    clone::Clone,
    convert::{AsMut, AsRef},
    default::Default,
    mem::size_of,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
    option::Option::{None, Some},
    ptr::NonNull,
    result::Result::Ok,
};

use alloc::{alloc_rros_shared::*, boxed::Box, rc::Rc, sync::Arc};
use kernel::{
    bindings, c_types,
    device::DeviceType,
    file::File,
    file_operations::{FileOpener, FileOperations},
    io_buffer::{IoBufferReader, IoBufferWriter, ReadableFromBytes, WritableToBytes},
    ioctl::{_IO, _IOR, _IOW, _IOWR},
    ktime::{timespec64_to_ktime, Timespec64},
    linked_list::{GetLinks, Links, List},
    memory_rros::RROS_SHARED_HEAP,
    pr_info,
    str::CStr,
    sync::{Lock, SpinLock},
    uidgid::{KgidT, KuidT},
    user_ptr::UserSlicePtr,
    Error, Result,
};

use crate::{
    clock::{rros_get_clock_by_fd, RrosClock},
    factory::{
        self, CloneData, FundleT, RrosElement, RrosElementIds, RrosFactory,
        __rros_get_element_by_fundle, rros_get_index, rros_index_factory_element, RrosFactoryType,
    },
    fifo::RROS_FIFO_MAX_PRIO,
    file::{rros_get_file, rros_put_file, RrosFile, RrosFileBinding},
    mutex::{
        atomic_cmpxchg, atomic_dec_return, atomic_inc, atomic_inc_return, atomic_read, atomic_set,
        RrosMutex, __rros_unlock_mutex, rros_commit_mutex_ceiling,
        rros_init_mutex_pi, rros_init_mutex_pp, rros_lock_mutex_timeout, rros_trylock_mutex,
        RROS_NO_HANDLE,
    },
    sched::{rros_schedule, RrosThread, RrosThreadWithLock, RrosTimeSpec},
    thread::{rros_current, rros_init_user_element, T_SIGNAL},
    timeout::{RrosTmode, RROS_INFINITE},
    wait::{RrosWaitQueue, RROS_WAIT_PRIO},
};

extern "C" {
    // FIXME: 这里要使用SpinLock代替
    pub fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> usize;

    pub fn rust_helper_raw_spin_unlock_irqrestore(
        lock: *mut bindings::hard_spinlock_t,
        flags: usize,
    );

    #[allow(dead_code)]
    pub fn rust_helper_raw_spin_lock(lock: *mut bindings::hard_spinlock_t);

    #[allow(dead_code)]
    pub fn rust_helper_raw_spin_unlock(lock: *mut bindings::hard_spinlock_t);

    pub fn rust_helper_raw_spin_lock_init(lock: *mut bindings::hard_spinlock_t);

    #[allow(improper_ctypes)]
    pub fn rust_helper_atomic_try_cmpxchg(
        v: bindings::atomic_t,
        old: *mut c_types::c_int,
        new: c_types::c_int,
    ) -> bool;

    #[allow(dead_code)]
    pub fn rust_helper_rcu_read_lock();

    #[allow(dead_code)]
    pub fn rust_helper_rcu_read_unlock();
}

fn raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> usize {
    unsafe {
        pr_info!("monitor.rs: raw_spin_lock_irqsave, lock is {:p}", lock);
        return rust_helper_raw_spin_lock_irqsave(lock);
    }
}

fn raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: usize) {
    unsafe {
        pr_info!("monitor.rs: raw_spin_unlock_irqrestore, lock is {:p}", lock);
        rust_helper_raw_spin_unlock_irqrestore(lock, flags);
    }
}

pub const CONFIG_RROS_NR_MONITORES: usize = 16;
const ONE_BILLION: i64 = 1000000000;

pub const RROS_MONITOR_EVENT: i32 = 0; /* Event monitor. */
pub const RROS_EVENT_GATED: i32 = 0; /* Gate protected. */
pub const RROS_EVENT_COUNT: i32 = 1; /* Semaphore. */
pub const RROS_EVENT_MASK: i32 = 2; /* Event (bit)mask. */
pub const RROS_MONITOR_GATE: i32 = 1; /* Gate monitor. */
pub const RROS_GATE_PI: i32 = 0; /* Gate with priority inheritance. */
pub const RROS_GATE_PP: i32 = 1; /* Gate with priority protection (ceiling). */

pub const RROS_MONITOR_NOGATE: u32 = 1;
pub const CLOCK_MONOTONIC: u32 = 1;
pub const CLOCK_REALTIME: u32 = 0;

#[allow(dead_code)]
const CONFIG_RROS_MONITOR: usize = 0;

const RROS_MONITOR_IOCBASE: u32 = 109; // 'm'

const RROS_MONIOC_ENTER: u32 = _IOW::<bindings::timespec64>(RROS_MONITOR_IOCBASE, 0);
const RROS_MONIOC_TRYENTER: u32 = _IO(RROS_MONITOR_IOCBASE, 1);
const RROS_MONIOC_EXIT: u32 = _IO(RROS_MONITOR_IOCBASE, 2);
const RROS_MONIOC_WAIT: u32 = _IOWR::<RrosMonitorWaitReq>(RROS_MONITOR_IOCBASE, 3);
const RROS_MONIOC_UNWAIT: u32 = _IOWR::<RrosMonitorUnWaitReq>(RROS_MONITOR_IOCBASE, 4);
const RROS_MONIOC_BIND: u32 = _IOR::<RrosMonitorBinding>(RROS_MONITOR_IOCBASE, 5);
const RROS_MONIOC_SIGNAL: u32 = _IOW::<i32>(RROS_MONITOR_IOCBASE, 6);

const RROS_MONITOR_SIGNALED: u32 = 0x1; /* Gate/Event */
const RROS_MONITOR_BROADCAST: u32 = 0x2; /* Event */
const RROS_MONITOR_TARGETED: u32 = 0x4; /* Event */

// NOTE: 这个应该放在timeout.rs中
const RROS_NONBLOCK: i64 = i64::MAX;

/*-----------------------------------------  uapi struct start  ---------------------------------------------*/
#[repr(C)]
#[derive(Copy, Clone, Default)]
struct RrosMonitorStateGate {
    owner: bindings::atomic_t,
    ceiling: u32,
    recursive_nesting: u32,
}

impl RrosMonitorStateGate {
    #[allow(dead_code)]
    fn recursive(&self) -> u32 {

        self.recursive_nesting & 0b1
    }

    #[allow(dead_code)]
    fn nesting(&self) -> u32 {
        (self.recursive_nesting & (!(0b1))) >> 1
    }
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct RrosMonitorStateEvent {
    value: bindings::atomic_t,
    pollrefs: bindings::atomic_t,
    gate_offset: u32,
}

// NOTE: in libevl's C side, this struct use union, so we should use union here.
#[repr(C)]
#[derive(Copy, Clone)]
union RrosMonitorStateU {
    gate: RrosMonitorStateGate,
    event: RrosMonitorStateEvent,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RrosMonitorState {
    flags: u32,
    u: RrosMonitorStateU,
}

impl RrosMonitorState {
    fn new_uninit() -> Box<MaybeUninit<Self>, RrosMemShared> {
        Box::<Self, RrosMemShared>::try_new_uninit_in(RrosMemShared).unwrap()
    }

    fn set_event_mask(&mut self, addval: i32) -> Result<i32> {
        let mut prev;
        let mut val;
        let mut next;

        val = unsafe { atomic_read(&mut self.u.event.value as *mut bindings::atomic_t) };

        loop {
            prev = val;
            next = prev | addval;
            val = unsafe {
                atomic_cmpxchg(
                    &mut self.u.event.value as *mut bindings::atomic_t,
                    prev,
                    next,
                )
            };
            if val == prev {
                break;
            }
        }

        Ok(next)
    }
}

#[repr(C)]
struct RrosMonitorWaitReq {
    timeout_ptr: u64,
    gatefd: i32,
    status: i32,
    value: i32,
}

unsafe impl ReadableFromBytes for RrosMonitorWaitReq {}
unsafe impl WritableToBytes for RrosMonitorWaitReq {}

#[repr(C)]
struct RrosMonitorUnWaitReq {
    gatefd: i32,
}

unsafe impl ReadableFromBytes for RrosMonitorUnWaitReq {}
unsafe impl WritableToBytes for RrosMonitorUnWaitReq {}

#[repr(C)]
struct RrosMonitorAttrs {
    pub clockfd: u32,
    pub type_protocol: u32,
    pub initval: u32,
}

impl RrosMonitorAttrs {
    fn type_foo(&self) -> u32 {
        self.type_protocol & 0b11
    }

    fn protocol(&self) -> u32 {
        (self.type_protocol & 0b111100) >> 2
    }
}

unsafe impl ReadableFromBytes for RrosMonitorAttrs {}
unsafe impl WritableToBytes for RrosMonitorAttrs {}

#[repr(C)]
struct RrosMonitorBinding {
    pub type_protocol: u32,
    pub eids: RrosElementIds,
}

impl Default for RrosMonitorBinding {
    fn default() -> Self {
        Self {
            type_protocol: 0,
            eids: RrosElementIds::default(),
        }
    }
}

impl RrosMonitorBinding {
    #[allow(dead_code)]
    fn type_foo(&self) -> u32 {
        self.type_protocol & 0b11
    }

    #[allow(dead_code)]
    fn protocol(&self) -> u32 {
        (self.type_protocol & 0b111100) >> 2
    }

    fn set_type_foo(&mut self, type_foo: u32) {
        self.type_protocol = self.type_protocol & ((!0b11) + type_foo);
    }

    fn set_protocol(&mut self, protocol: u32) {
        self.type_protocol = self.type_protocol & ((!0b111100) + protocol);
    }
}

unsafe impl ReadableFromBytes for RrosMonitorBinding {}
unsafe impl WritableToBytes for RrosMonitorBinding {}

/*----------------------------------------  uapi struct end  --------------------------------------------*/

/*---------------------------------------  helper function  ------------------------------------------------- */
#[allow(dead_code)]
fn get_monitor_element_by_fundle(
    fac: &'static mut SpinLock<RrosFactory>,
    fundle: factory::FundleT,
) -> *mut RrosMonitor {
    let map = unsafe { &mut *(fac.locked_data().get()) };
    let map = map.inside.as_mut().unwrap().index.as_mut().unwrap();
    let e = factory::__rros_get_element_by_fundle(map, fundle);
    unsafe { (*e).pointer as *mut RrosMonitor }
}

pub fn rros_is_mutex_owner(fastlock: *mut bindings::atomic_t, ownerh: FundleT) -> bool {
    rros_get_index(atomic_read(fastlock) as u32) == ownerh
}

pub fn __rros_commit_monitor_ceiling() {
    let curr = rros_current();
    let curr_ref = unsafe {
        let curr_ptr = (*curr).locked_data().get();
        &mut *curr_ptr
    };

    let map = unsafe { &mut *RROS_MONITOR_FACTORY.locked_data().get() };
    let map = map.inside.as_mut().unwrap().index.as_mut().unwrap();

    let e = __rros_get_element_by_fundle(map, curr_ref.u_window.as_ref().unwrap().pp_pending);

    let gate = unsafe { (*e).pointer as *mut RrosMonitor };
    if gate.is_null() {
        curr_ref.u_window.as_mut().unwrap().pp_pending = RROS_NO_HANDLE;
        return;
    }

    let gate = unsafe { &mut *gate };
    if gate.protocol == RROS_GATE_PP {
        let gate_core = if let RrosMonitorCore::Gate(gate) = &mut gate.core {
            gate
        } else {
            panic!("invalid RrosMonitorCore value");
        };
        let mutex = Arc::as_ptr(&gate_core.mutex) as *mut RrosMutex;
        rros_commit_mutex_ceiling(mutex).unwrap();
    }
    // TODO evl_put_element
    // evl_put_element();
    curr_ref.u_window.as_mut().unwrap().pp_pending = RROS_NO_HANDLE;
    return;
}

pub fn rros_commit_monitor_ceiling() {
    let curr = rros_current();
    let curr_ref = unsafe {
        let curr_ptr = (*curr).locked_data().get();
        &mut *curr_ptr
    };
    if curr_ref.u_window.as_ref().unwrap().pp_pending != RROS_NO_HANDLE {
        __rros_commit_monitor_ceiling();
    }
}

/*---------------------------------------  helper function end  ----------------------------------------------- */

struct RrosMonitorGate {
    pub mutex: Arc<RrosMutex>,
    pub events: List<Arc<RefCellEventWrapper>>,
    pub lock: bindings::hard_spinlock_t,
}

impl RrosMonitorGate {
    // FIXME: return value shoule be Result<Self>, because `Arc:::try_new().unwrap()` may panic.
    pub fn new() -> Self {
        Self {
            mutex: Arc::try_new(RrosMutex::new()).unwrap(),
            events: List::new(),
            lock: bindings::hard_spinlock_t::default(),
        }
    }

    pub fn __enter_monitor(&mut self, ts64: Option<&mut Timespec64>) -> Result<i32> {
        let mut timeout = RROS_INFINITE;
        let tmode = if timeout != 0 {
            RrosTmode::RrosAbs
        } else {
            RrosTmode::RrosRel
        };
        if ts64.is_some() {
            timeout = timespec64_to_ktime(*ts64.unwrap());
        }
        let mutex = Arc::as_ptr(&self.mutex) as *mut RrosMutex;
        return rros_lock_mutex_timeout(mutex, timeout, tmode);
    }

    pub fn enter_monitor(&mut self, ts64: Option<&mut Timespec64>) -> Result<i32> {
        let curr = rros_current();
        let curr = unsafe {
            let curr_ptr = (*curr).locked_data().get();
            &mut *curr_ptr
        };

        if rros_is_mutex_owner(
            self.mutex.fastlock,
            curr.element.as_ref().borrow().deref().fundle,
        ) {
            return Err(Error::EDEADLK);
        }

        rros_commit_monitor_ceiling();
        return self.__enter_monitor(ts64);
    }

    pub fn tryenter_monitor(&mut self) -> Result<i32> {
        rros_commit_monitor_ceiling();
        let mutex = Arc::as_ptr(&self.mutex) as *mut RrosMutex;
        return rros_trylock_mutex(mutex);
    }

    pub fn __exit_monitor(&mut self, fundle: u32, curr: &mut RrosThread) {
        let pp_pending = &mut curr.u_window.as_mut().unwrap().pp_pending;
        if fundle == *pp_pending {
            *pp_pending = RROS_NO_HANDLE;
        }
        
        let mutex = Arc::as_ptr(&self.mutex) as *mut RrosMutex;
        __rros_unlock_mutex(mutex).unwrap();
    }

    pub fn exit_monitor(
        &mut self,
        fundle: u32,
        state: &mut Box<RrosMonitorState, RrosMemShared>,
    ) -> Result<i32> {
        let curr = rros_current();
        let curr = unsafe { &mut *((*curr).locked_data().get()) };
        //if (!evl_is_mutex_owner(gate->mutex.fastlock, fundle_of(curr)))
        // return -EPERM;
        if !rros_is_mutex_owner(
            self.mutex.fastlock,
            curr.element.as_ref().borrow().deref().fundle,
        ) {
            return Err(Error::EPERM);
        }

        let flags =
            raw_spin_lock_irqsave(&mut self.lock as *mut bindings::hard_spinlock_t);
        self.__exit_monitor(fundle, curr);
        // FIXME: hack_self
        let hack_self = self as *mut RrosMonitorGate;
        if (state.flags & RROS_MONITOR_SIGNALED) != 0 {
            state.flags &= !RROS_MONITOR_SIGNALED;
            let mut cursor = unsafe { (*hack_self).events.cursor_front_mut() };
            // NOTE: 'entry' has the borrow ref of cursor, so if use 'entry' and 'cussor.foo()' the same time, we should use '{}' to wrap the 'entry'.
            while let Some(entry) = cursor.current() {
                {
                    let mut event = entry.inner.borrow_mut();
                    let event = event.deref_mut();
                    {
                        event.wait_queue.lock.raw_spin_lock();
                    }
                    unsafe {
                        if ((*entry.state).flags & RROS_MONITOR_SIGNALED) != 0 {
                            let arc_event: Arc<RefCellEventWrapper> = {
                                Arc::increment_strong_count(
                                    entry as *const _ as *mut RefCellEventWrapper,
                                );
                                Arc::from_raw(entry as *const _ as *mut RefCellEventWrapper)
                            };
                            event.wakeup_waiters(state, &mut self.events, arc_event);
                        }
                    }
                    {
                        event.wait_queue.lock.raw_spin_unlock();
                    }
                }
                cursor.move_next();
            }
        }

        unsafe {
            raw_spin_unlock_irqrestore(&mut self.lock as *mut bindings::hard_spinlock_t, flags);
            rros_schedule();
        }
        Ok(0)
    }
}

struct RrosMonitorEvent {
    pub wait_queue: RrosWaitQueue,
    pub next: Links<RefCellEventWrapper>,
    pub gate: Option<u32>, // gate: Option<Arc<RefCell<RrosMonitorGate>>>,
                           // TODO: monitor-poll hasn't been used
                           // poll_head: List<>
                           // next_poll: Links<>
}

impl RrosMonitorEvent {
    pub fn new(clock: &mut RrosClock, flags: i32) -> Self {
        Self {
            wait_queue: RrosWaitQueue::new(clock as *mut RrosClock, flags),
            next: Links::new(),
            gate: None,
        }
    }

    pub fn untrack_event(
        &mut self,
        arc_self: Arc<RefCellEventWrapper>,
        state: &mut Box<RrosMonitorState, RrosMemShared>,
    ) {
        let gatefd = self.gate.unwrap();
        let mut gate = get_monitor_by_fd(gatefd as i32).unwrap().0;
        let gate_core_ref = if let RrosMonitorCore::Gate(gate_core) = &mut gate.core {
            gate_core
        } else {
            panic!("invalid RrosMonitorCore value");
        };
        let flags;
        {
            flags =
                raw_spin_lock_irqsave(&mut gate_core_ref.lock as *mut bindings::hard_spinlock_t);
            self.wait_queue.lock.raw_spin_lock();
        }
        let list = &mut gate_core_ref.events;
        self.__untrack_event(list, arc_self, state);

        {
            self.wait_queue.lock.raw_spin_unlock();
            raw_spin_unlock_irqrestore(
                &mut gate_core_ref.lock as *mut bindings::hard_spinlock_t,
                flags,
            )
        }
        Box::into_raw(gate);
    }

    pub fn __untrack_event(
        &mut self,
        list: &mut List<Arc<RefCellEventWrapper>>,
        arc_self: Arc<RefCellEventWrapper>,
        state: &mut Box<RrosMonitorState, RrosMemShared>,
    ) {
        if !self.wait_queue.is_active() {
            unsafe {
                list.remove(&arc_self);
            }
            self.gate = None;
            {
                state.u.event.gate_offset = RROS_MONITOR_NOGATE;
            }
        }
    }

    pub fn wakeup_waiters(
        &mut self,
        state: &mut Box<RrosMonitorState, RrosMemShared>,
        list: &mut List<Arc<RefCellEventWrapper>>,
        arc_self: Arc<RefCellEventWrapper>,
    ) {
        let state = &mut *state ;
        let bcast = state.flags & RROS_MONITOR_BROADCAST;

        if self.wait_queue.is_active() {
            if bcast == 0 {
                self.wait_queue.flush_locked(0);
            } else if (state.flags & RROS_MONITOR_TARGETED) != 0 {
                // FIXME: hack_self
                let hack_self = self as *mut RrosMonitorEvent;
                let mut cursor =
                    unsafe { (*hack_self).wait_queue.wchan.wait_list.cursor_front_mut() };
                while let Some(waiter) = cursor.current() {
                    if (waiter.get_info() & T_SIGNAL) != 0 {
                        let waiter = unsafe {
                            RrosThreadWithLock::transmute_to_self(Arc::from_raw(
                                waiter as *const _ as *const SpinLock<RrosThread>,
                            ))
                        };
                        self.wait_queue.wake_up(Some(waiter), 0);
                    }
                    cursor.move_next();
                }
            } else {
                self.wait_queue.wake_up_head();
            }
            self.__untrack_event(list, arc_self, state);
        }

        state.flags &= !(RROS_MONITOR_SIGNALED | RROS_MONITOR_BROADCAST | RROS_MONITOR_TARGETED);
    }

    pub fn signal_monitor_ungated(
        &mut self,
        protocol: i32,
        sigval: i32,
        state: &mut Box<RrosMonitorState, RrosMemShared>,
    ) -> Result<i32> {
        let mut pollable: bool = true;
        let flags: u64;
        let val: i32;

        match protocol {
            RROS_EVENT_COUNT => {
                flags = self.wait_queue.lock.raw_spin_lock_irqsave();

                if (unsafe {
                    atomic_inc_return(&mut state.u.event.value as *mut bindings::atomic_t)
                } <= 0)
                {
                    self.wait_queue.wake_up_head();
                    pollable = false;
                }

                self.wait_queue.lock.raw_spin_unlock_irqrestore(flags);
            }
            RROS_EVENT_MASK => {
                flags = self.wait_queue.lock.raw_spin_lock_irqsave();

                val = state.set_event_mask(sigval).unwrap();
                if val != 0 {
                    self.wait_queue.flush_locked(0);
                } else {
                    pollable = false;
                }
                self.wait_queue.lock.raw_spin_unlock_irqrestore(flags);
            }
            _ => {
                return Err(Error::EINVAL);
            }
        }
        if pollable {
            //TODO: rros_signal_poll_events(event.item., events)
        }
        unsafe {
            rros_schedule();
        }
        Ok(0)
    }
}

struct RefCellEventWrapper {
    pub inner: RefCell<RrosMonitorEvent>,
    // HACK: temporary unsafe hack
    pub state: *mut RrosMonitorState,
}

impl GetLinks for RefCellEventWrapper {
    type EntryType = RefCellEventWrapper;

    fn get_links(data: &Self::EntryType) -> &Links<Self::EntryType> {
        unsafe { &(*data.inner.as_ptr()).next }
    }
}

enum RrosMonitorCore {
    Gate(RrosMonitorGate),
    Event(Arc<RefCellEventWrapper>),
}

struct RrosMonitor {
    pub element: Rc<RefCell<RrosElement>>,
    pub state: Box<RrosMonitorState, RrosMemShared>,
    pub type_foo: i32,
    pub protocol: i32,
    pub core: RrosMonitorCore,
    #[allow(dead_code)]
    pub e_pointer: *mut RrosElement,

}

impl RrosMonitor {
    pub fn new() -> Box<Self> {
        Box::try_new(Self {
            element: Rc::try_new(RefCell::new(RrosElement::new().unwrap())).unwrap(),
            state: unsafe { RrosMonitorState::new_uninit().assume_init() },
            type_foo: 0,
            protocol: 0,
            core: RrosMonitorCore::Gate(RrosMonitorGate::new()),
            e_pointer: 0 as *mut RrosElement,
        })
        .unwrap()
    }

    #[allow(dead_code)]
    pub fn new_uninit() -> Box<MaybeUninit<Self>> {
        Box::<Self>::try_new_uninit().unwrap()
    }

    pub fn set_core(&mut self, core: RrosMonitorCore) {
        self.core = core
    }
}

// NOTE: `monitor_factory_build`'s return needs `Result<>` to handle errors
fn monitor_factory_build(
    fac: &'static mut SpinLock<RrosFactory>,
    uname: &'static CStr,
    u_attrs: Option<*mut u8>,
    clone_flags: i32,
    state_offp: &mut u32,
) -> Rc<RefCell<RrosElement>> {
    let mut reader = unsafe {
        UserSlicePtr::new(
            u_attrs.unwrap() as *mut c_types::c_void,
            size_of::<RrosMonitorAttrs>(),
        )
        .reader()
    };
    let attrs = reader.read::<RrosMonitorAttrs>().unwrap();

    // check attrs
    match attrs.type_foo() as i32 {
        RROS_MONITOR_GATE => match attrs.protocol() as i32 {
            RROS_GATE_PP => {
                if attrs.initval == 0 || attrs.initval > RROS_FIFO_MAX_PRIO as u32 {
                    pr_info!("invalid attrs.initval value");
                }
            }
            RROS_GATE_PI => {
                if attrs.initval != 0 {
                    pr_info!("invalid attrs.initval value");
                }
            }
            _ => {
                pr_info!("invalid attrs.protocol value");
            }
        },
        RROS_MONITOR_EVENT => match attrs.protocol() as i32 {
            RROS_EVENT_GATED | RROS_EVENT_COUNT | RROS_EVENT_MASK => (),
            _ => {
                pr_info!("invalid attrs.protocol value");
            }
        },
        _ => {
            pr_info!("invalid attrs.type_foo value");
        }
    }
    let res_clock = rros_get_clock_by_fd(attrs.clockfd as i32);
    let clock = if let Ok(_clock) = res_clock {
        _clock
    } else {
        panic!("invalid RrosClock value");
    };
    let mut monitor = RrosMonitor::new();
    monitor.e_pointer = monitor.element.clone().as_ref().as_ptr();

    let mut _ret = rros_init_user_element(monitor.element.clone(), fac, uname, clone_flags);

    match attrs.type_foo() as i32 {
        RROS_MONITOR_GATE => {
            monitor.set_core(RrosMonitorCore::Gate(RrosMonitorGate::new()));
            let gate = if let RrosMonitorCore::Gate(gate) = &mut monitor.core {
                gate
            } else {
                panic!("invalid RrosMonitor");
            };
            match attrs.protocol() as i32 {
                RROS_GATE_PP => {
                    monitor.state.u.gate.ceiling = attrs.initval;
                    unsafe {
                        let mutex = Arc::as_ptr(&gate.mutex) as *mut RrosMutex;
                        rros_init_mutex_pp(
                            mutex,
                            clock as *mut RrosClock,
                            &mut monitor.state.u.gate.owner as *mut bindings::atomic_t,
                            &mut monitor.state.u.gate.ceiling as *mut u32,
                        );
                    }
                }
                RROS_GATE_PI => unsafe {
                    let mutex = Arc::as_ptr(&gate.mutex) as *mut RrosMutex;
                    rros_init_mutex_pi(
                        mutex,
                        clock as *mut RrosClock,
                        &mut monitor.state.u.gate.owner as *mut bindings::atomic_t,
                    );
                },
                _ => {}
            }
            unsafe {
                rust_helper_raw_spin_lock_init(&mut gate.lock as *mut bindings::hard_spinlock_t);
            }
        }
        RROS_MONITOR_EVENT => {
            let state_ptr = Box::into_raw(monitor.state);
            monitor.state = unsafe { Box::from_raw_in(state_ptr, RrosMemShared) };
            monitor.set_core(RrosMonitorCore::Event(
                Arc::try_new(RefCellEventWrapper {
                    inner: RefCell::new(RrosMonitorEvent::new(clock, RROS_WAIT_PRIO as i32)),
                    state: state_ptr,
                })
                .unwrap(),
            ));
            unsafe {
                monitor.state.u.event.gate_offset = RROS_MONITOR_NOGATE;
                atomic_set(
                    &mut monitor.state.u.event.value as *mut bindings::atomic_t,
                    attrs.initval as i32,
                );
            }
        }
        _ => {}
    }
    monitor.type_foo = attrs.type_foo() as i32;
    monitor.protocol = attrs.protocol() as i32;
    let state_ptr = Box::into_raw(monitor.state);
    // #Safety
    // static mut value
    *state_offp =
        unsafe { RROS_SHARED_HEAP.rros_shared_offset(state_ptr as *mut c_types::c_void) as u32 };
    // #Safety
    // the state_ptr come from into_raw call.
    monitor.state = unsafe { Box::from_raw_in(state_ptr, RrosMemShared) };

    let e: Rc<RefCell<RrosElement>> = monitor.element.clone();
    rros_index_factory_element(e.clone());
    let monitor_ptr = Box::into_raw(monitor);
    let monitor = unsafe { Box::from_raw(monitor_ptr) };
    monitor.element.deref().borrow_mut().deref_mut().pointer = monitor_ptr as *mut u8;
    // NOTE: Box::into_raw consumes the Box.
    // After calling this function, the caller is responsible for the memory previously managed by the Box.
    Box::into_raw(monitor);

    return e;
}

fn monitor_factory_dispose(_ele: factory::RrosElement) {}

pub static mut RROS_MONITOR_FACTORY: SpinLock<factory::RrosFactory> = unsafe {
    SpinLock::new(factory::RrosFactory {
        name: CStr::from_bytes_with_nul_unchecked("monitor\0".as_bytes()),
        // fops: Some(&MonitorOps),
        nrdev: CONFIG_RROS_NR_MONITORES,
        build: Some(monitor_factory_build),
        dispose: Some(monitor_factory_dispose),
        attrs: None, //sysfs::attribute_group::new(),
        flags: RrosFactoryType::CLONE,
        inside: Some(factory::RrosFactoryInside {
            type_: DeviceType::new(),
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

fn get_monitor_by_fd(efd: i32) -> Result<(Box<RrosMonitor>, NonNull<RrosFile>)> {
    let ret = rros_get_file(efd as u32);
    match ret {
        None => return Err(Error::EINVAL),
        Some(rfile_nonnull) => {
            let filp_ref = unsafe { &mut *(rfile_nonnull.as_ref().filp) };
            let __fbind: *mut RrosFileBinding = filp_ref.private_data as *mut RrosFileBinding;
            let monitor =
                unsafe { Box::from_raw((*((*__fbind).element)).pointer as *mut RrosMonitor) };
            return Ok((monitor, rfile_nonnull));
        }
    }
}

fn test_event_mask(state: &mut RrosMonitorState, r_value: &mut i32) -> bool {
    let mut val: i32;
    loop {
        unsafe {
            val = atomic_read(&mut state.u.event.value as *mut bindings::atomic_t);
        }
        if val == 0 {
            return false;
        }
        if (unsafe { atomic_cmpxchg(&mut state.u.event.value as *mut bindings::atomic_t, val, 0) }
            == val)
        {
            *r_value = val;
            return true;
        }
    }
}

// NOTE: borrow_mut()的BUG -> panic, 不能稳定触发
fn wait_monitor_ungated(
    event_ref: &mut RrosMonitor,
    filp: &File,
    _req: &mut RrosMonitorWaitReq,
    ts64: &mut Timespec64,
    r_value: &mut i32,
) -> i32 {
    let state_ref = event_ref.state.as_mut();
    let event_core = if let RrosMonitorCore::Event(event) = &mut event_ref.core {
        event.clone()
    } else {
        panic!("invalid RrosMonitorCore value");
    };
    let tmode;
    let mut timeout = timespec64_to_ktime(*ts64);
    if timeout == 0 {
        tmode = RrosTmode::RrosRel;
    } else {
        tmode = RrosTmode::RrosAbs;
    }
    let mut ret = 0;
    let mut val;
    {
        match event_ref.protocol {
            RROS_EVENT_COUNT => {
                let at = unsafe { &mut state_ref.u.event.value };
                if !filp.is_blocking() {
                    val = atomic_read(at);
                    loop {
                        if val <= 0 {
                            ret = Error::EAGAIN.to_kernel_errno();
                            break;
                        }
                        if unsafe {
                            rust_helper_atomic_try_cmpxchg(
                                *at,
                                &mut val as *mut c_types::c_int,
                                val - 1,
                            )
                        } {
                            break;
                        }
                    }
                } else {
                    let flags = event_core
                        .deref()
                        .inner
                        .borrow_mut()
                        .deref_mut()
                        .wait_queue
                        .lock
                        .raw_spin_lock_irqsave();
                    if atomic_dec_return(at) < 0 {
                        {
                            event_core
                                .deref()
                                .inner
                                .borrow_mut()
                                .deref_mut()
                                .wait_queue
                                .locked_add(timeout, tmode);
                        }
                        event_core
                            .deref()
                            .inner
                            .borrow_mut()
                            .deref_mut()
                            .wait_queue
                            .lock
                            .raw_spin_unlock_irqrestore(flags);
                        // FIXME: temporary hack
                        let ptr_hack = {
                            let mut tmp = event_core.deref().inner.borrow_mut();
                            tmp.deref_mut() as *mut RrosMonitorEvent
                        };

                        unsafe {
                            ret = (*ptr_hack).wait_queue.wait_schedule();
                        }
                        // {
                        //     let mut refmut = event_core.deref().inner.borrow_mut();
                        //     ret = refmut.deref_mut().wait_queue.wait_schedule();
                        //     drop(refmut);
                        // }
                        if ret != 0 {
                            atomic_inc(at);
                        }
                    } else {
                        event_core
                            .deref()
                            .inner
                            .borrow_mut()
                            .deref_mut()
                            .wait_queue
                            .lock
                            .raw_spin_unlock_irqrestore(flags);
                    }
                }
            }
            RROS_EVENT_MASK => {
                if !filp.is_blocking() {
                    timeout = RROS_NONBLOCK;
                }
                let get_cond = || test_event_mask(state_ref, r_value);
                let ptr_hack = {
                    let mut tmp = event_core.deref().inner.borrow_mut();
                    tmp.deref_mut() as *mut RrosMonitorEvent
                };
                unsafe {
                    ret = (*ptr_hack)
                        .wait_queue
                        .wait_timeout(timeout, tmode, get_cond);
                }
                // ret = event_core.deref().inner.borrow_mut().deref_mut().wait_queue.wait_timeout(timeout, tmode, get_cond);
                if ret == 0 {
                    unsafe {
                        rros_schedule();
                    }
                }
            }
            _ => {}
        }
    }
    return ret;
}


fn wait_monitor(
    event_ref: &mut RrosMonitor,
    _file: &File,
    req: &mut RrosMonitorWaitReq,
    ts64: &mut Timespec64,
    r_op_ret: &mut i32,
    r_value: &mut i32,
) -> i32 {
    let curr = rros_current();
    let curr_ref = unsafe { &mut *((*curr).locked_data().get()) };
    let mut ret = 0;
    let mut op_ret = 0;
    let tmode: RrosTmode;
    let flags: usize;
    if event_ref.type_foo != RROS_MONITOR_EVENT {
        op_ret = Error::EINVAL.to_kernel_errno();
        *r_op_ret = op_ret;
        return ret;
    }

    let timeout = timespec64_to_ktime(*ts64);
    if timeout != 0 {
        tmode = RrosTmode::RrosAbs;
    } else {
        tmode = RrosTmode::RrosRel;
    }

    if req.gatefd < 0 {
        ret = wait_monitor_ungated(event_ref, _file, req, ts64, r_value);
        *r_op_ret = ret;
        return ret;
    }

    let res = get_monitor_by_fd(req.gatefd).unwrap();
    let (mut gate, mut efilp) = res;

    // type_foo check
    if gate.type_foo != RROS_MONITOR_GATE {
        op_ret = Error::EINVAL.to_kernel_errno();
        rros_put_file(unsafe { efilp.as_mut() }).unwrap();
        *r_op_ret = op_ret;
        Box::into_raw(gate);
        return ret;
    }

    let gate_core_ref = if let RrosMonitorCore::Gate(gate) = &mut gate.core {
        gate
    } else {
        panic!("invalid RrosMonitorCore value");
    };

    // owner of mutex check
    if !rros_is_mutex_owner(
        gate_core_ref.mutex.fastlock,
        curr_ref.element.as_ref().borrow().deref().fundle,
    ) {
        op_ret = Error::EPERM.to_kernel_errno();
        rros_put_file(unsafe { efilp.as_mut() }).unwrap();
        *r_op_ret = op_ret;
        Box::into_raw(gate);
        return ret;
    }
    let state = &mut event_ref.state;
    let event_core = if let RrosMonitorCore::Event(event) = &mut event_ref.core {
        event.clone()
    } else {
        panic!("invalid RrosMonitorCore value");
    };
    // let mut event_core_refmut = event_core.deref().inner.borrow_mut().deref_mut();
    // let event_core_ref = event_core_refmut.deref_mut();

    let gate_core_ref = if let RrosMonitorCore::Gate(gate) = &mut gate.core {
        gate
    } else {
        panic!("invalid RrosMonitorCore value");
    };

    flags = {
        raw_spin_lock_irqsave(&mut gate_core_ref.lock as *mut bindings::hard_spinlock_t)
    };
    event_core
        .deref()
        .inner
        .borrow_mut()
        .deref_mut()
        .wait_queue
        .lock
        .raw_spin_lock();

    {
        if event_core
            .deref()
            .inner
            .borrow_mut()
            .deref_mut()
            .gate
            .is_none()
        {
            // Case: event's gate is none
            // add event on gate's events list
            gate_core_ref.events.push_back(event_core.clone());
            event_core.deref().inner.borrow_mut().deref_mut().gate = Some(req.gatefd as u32);
            unsafe {
                state.u.event.gate_offset = RROS_SHARED_HEAP.rros_shared_offset(gate.state.as_mut()
                    as *mut RrosMonitorState
                    as *mut c_types::c_void) as u32;
            }
        } else if event_core
            .deref()
            .inner
            .borrow_mut()
            .deref_mut()
            .gate
            .unwrap()
            != req.gatefd as u32
        {
            // Case: event's gate is not the gate in user's req
            unsafe {
                event_core
                    .deref()
                    .inner
                    .borrow_mut()
                    .deref_mut()
                    .wait_queue
                    .lock
                    .raw_spin_unlock();

                raw_spin_unlock_irqrestore(
                    &mut gate_core_ref.lock as *mut bindings::hard_spinlock_t,
                    flags,
                );
                op_ret = Error::EBADFD.to_kernel_errno();
                rros_put_file(efilp.as_mut()).unwrap();
                *r_op_ret = op_ret;
                Box::into_raw(gate);
                return 0;
            }
        }
    }

    {
        event_core
            .deref()
            .inner
            .borrow_mut()
            .deref_mut()
            .wait_queue
            .locked_add(timeout, tmode);
    }
    unsafe {
        let curr_ref = &mut *(*curr).locked_data().get();
        curr_ref.lock.raw_spin_lock();
        (*curr_ref.rq.unwrap()).lock.raw_spin_lock();
        curr_ref.info &= !T_SIGNAL;
        curr_ref.lock.raw_spin_unlock();
        (*curr_ref.rq.unwrap()).lock.raw_spin_unlock();
        event_core
            .deref()
            .inner
            .borrow_mut()
            .deref_mut()
            .wait_queue
            .lock
            .raw_spin_unlock();
        let fundle = gate.element.deref().borrow().deref().fundle;
        // unlock the lock of gate mutex
        gate_core_ref.__exit_monitor(fundle, curr_ref);
        raw_spin_unlock_irqrestore(
            &mut gate_core_ref.lock as *mut bindings::hard_spinlock_t,
            flags,
        );
    }
    let ptr_hack = {
        let mut tmp = event_core.deref().inner.borrow_mut();
        tmp.deref_mut() as *mut RrosMonitorEvent
    };
    // FIXME: temporary hack
    unsafe {
        ret = (*ptr_hack).wait_queue.wait_schedule();
    }
    // ret = event_core_ref.wait_queue.wait_schedule();
    if ret != 0 {
        let ptr_hack = {
            let mut tmp = event_core.deref().inner.borrow_mut();
            tmp.deref_mut() as *mut RrosMonitorEvent
        };
        unsafe {
            (*ptr_hack).untrack_event(event_core.clone(), state);
        }
        // event_core.deref().inner.borrow_mut().deref_mut().untrack_event(event_core.clone(), state);
        if ret == Error::EINTR.to_kernel_errno() as i32 {
            unsafe {
                rros_put_file(efilp.as_mut()).unwrap();
            }
            *r_op_ret = op_ret;
            Box::into_raw(gate);
            return ret;
        }
        op_ret = ret;
    }

    if ret != Error::EIDRM.to_kernel_errno() as i32 {
        ret = gate_core_ref.__enter_monitor(None).unwrap();
    }

    unsafe {
        rros_put_file(efilp.as_mut()).unwrap();
    }
    *r_op_ret = op_ret;
    Box::into_raw(gate);
    return ret;
}

fn unwait_monitor(req: &mut RrosMonitorUnWaitReq) -> Result<i32> {
    let ret = get_monitor_by_fd(req.gatefd).unwrap();
    let (mut gate, mut efilp) = ret;
    let gate_core_ref = if let RrosMonitorCore::Gate(gate_core) = &mut gate.core {
        gate_core
    } else {
        panic!("invalid RrosMonitorCore valie");
    };

    let ret = gate_core_ref.enter_monitor(None);
    rros_put_file(unsafe { efilp.as_mut() }).unwrap();
    Box::into_raw(gate);

    return ret;
}

pub struct MonitorOps {}

impl MonitorOps {
    fn monitor_common_ioctl(
        _file: &kernel::file::File,
        _cmd: &mut kernel::file_operations::IoctlCommand,
    ) -> kernel::prelude::Result<i32> {
        let mut monitor = unsafe {
            let __fbind = (*_file.get_ptr()).private_data as *mut RrosFileBinding;
            Box::from_raw((*((*__fbind).element)).pointer as *mut RrosMonitor)
        };
        let event_core = if let RrosMonitorCore::Event(event) = &mut monitor.core {
            event.clone()
        } else {
            panic!("invalid RrosMonitorCore value");
        };

        let ptr_hack = {
            let mut tmp = event_core.deref().inner.borrow_mut();
            tmp.deref_mut() as *mut RrosMonitorEvent
        };
        // let mut event_core_refmut = event_core.deref().inner.borrow_mut();
        // let event_core_ref = event_core_refmut.deref_mut();
        let sigval: i32;
        let ret;
        match _cmd.cmd {
            RROS_MONIOC_SIGNAL => {
                let mut reader = unsafe {
                    UserSlicePtr::new(_cmd.arg as *mut c_types::c_void, size_of::<i32>()).reader()
                };
                let res = reader.read::<i32>();
                match res {
                    Err(_e) => {
                        Box::into_raw(monitor);
                        return Err(Error::EFAULT);
                    }
                    Ok(ret_sigval) => {
                        sigval = ret_sigval;
                    }
                }
                ret = unsafe {
                    (*ptr_hack).signal_monitor_ungated(monitor.protocol, sigval, &mut monitor.state)
                };
            }
            _ => {
                Box::into_raw(monitor);
                return Err(Error::ENOTTY);
            }
        }
        Box::into_raw(monitor);
        return ret;
    }
}

impl FileOpener<u8> for MonitorOps {
    fn open(shared: &u8, _file: &File) -> Result<Self::Wrapper> {
        let mut data = CloneData::default();
        unsafe {
            data.ptr = shared as *const u8 as *mut u8;
            let a = KuidT((*(shared as *const u8 as *const bindings::inode)).i_uid);
            let b = KgidT((*(shared as *const u8 as *const bindings::inode)).i_gid);
            (*RROS_MONITOR_FACTORY.locked_data().get())
                .inside
                .as_mut()
                .unwrap()
                .kuid = Some(a);
            (*RROS_MONITOR_FACTORY.locked_data().get())
                .inside
                .as_mut()
                .unwrap()
                .kgid = Some(b);
        }
        // bindings::stream_open();
        Ok(Box::try_new(data)?)
    }
}

impl FileOperations for MonitorOps {
    kernel::declare_file_operations!(ioctl, oob_ioctl);
    type Wrapper = Box<CloneData>;

    fn ioctl(
        _this: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as Deref>::Target,
        _file: &kernel::file::File,
        _cmd: &mut kernel::file_operations::IoctlCommand,
    ) -> kernel::prelude::Result<i32> {
        let monitor = unsafe {
            let __fbind = (*_file.get_ptr()).private_data as *mut RrosFileBinding;
            Box::from_raw((*((*__fbind).element)).pointer as *mut RrosMonitor)
        };

        if _cmd.cmd != RROS_MONIOC_BIND {
            Box::into_raw(monitor);
            return Self::monitor_common_ioctl(_file, _cmd);
        }
        let mut bind = RrosMonitorBinding::default();
        let u_bind: *mut RrosMonitorBinding;

        bind.set_type_foo(monitor.type_foo as u32);
        bind.set_protocol(monitor.protocol as u32);
        bind.eids
            .set_minor(monitor.element.deref().borrow().deref().minor as u32).unwrap();
        bind.eids
            .set_fundle(monitor.element.deref().borrow().deref().fundle as u32).unwrap();
        u_bind = _cmd.arg as *mut RrosMonitorBinding;

        let mut writer = unsafe {
            UserSlicePtr::new(
                u_bind as *mut c_types::c_void,
                size_of::<RrosMonitorBinding>(),
            )
            .writer()
        };
        let _ret = writer.write::<RrosMonitorBinding>(&bind);

        Box::into_raw(monitor);
        return Ok(0);
    }

    fn oob_ioctl(
        _this: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as Deref>::Target,
        _file: &kernel::file::File,
        _cmd: &mut kernel::file_operations::IoctlCommand,
    ) -> kernel::prelude::Result<i32> {
        let mut monitor = unsafe {
            let __fbind = (*_file.get_ptr()).private_data as *mut RrosFileBinding;
            Box::from_raw((*((*__fbind).element)).pointer as *mut RrosMonitor)
        };
        let mut uwreq: RrosMonitorUnWaitReq;
        let u_uwreq: *mut RrosMonitorUnWaitReq;
        let mut wreq: RrosMonitorWaitReq;
        let u_wreq: *mut RrosMonitorWaitReq;
        let u_uts: *mut RrosTimeSpec;
        let uts: RrosTimeSpec;
        let mut ts64;
        let mut op_ret: i32 = 0;
        let mut value: i32 = 0;
        let ret;

        if _cmd.cmd == RROS_MONIOC_WAIT {
            u_wreq = _cmd.arg as *mut RrosMonitorWaitReq;
            let mut reader = unsafe {
                UserSlicePtr::new(
                    u_wreq as *mut c_types::c_void,
                    size_of::<RrosMonitorWaitReq>(),
                )
                .reader()
            };
            let res = reader.read::<RrosMonitorWaitReq>();
            match res {
                Err(_e) => {
                    Box::into_raw(monitor);
                    return Err(Error::EFAULT);
                }
                Ok(ret_wreq) => {
                    wreq = ret_wreq;
                }
            }
            u_uts = wreq.timeout_ptr as *mut RrosTimeSpec;
            let mut reader = unsafe {
                UserSlicePtr::new(u_uts as *mut c_types::c_void, size_of::<RrosTimeSpec>())
                    .reader()
            };
            let res = reader.read::<RrosTimeSpec>();
            match res {
                Err(_e) => {
                    Box::into_raw(monitor);
                    return Err(Error::EFAULT);
                }
                Ok(ret_uts) => {
                    uts = ret_uts;
                }
            }
            if uts.tv_nsec >= ONE_BILLION {
                Box::into_raw(monitor);
                return Err(Error::EINVAL);
            }
            ts64 = Timespec64::new(uts.tv_sec, uts.tv_nsec);
            let ret = wait_monitor(
                monitor.as_mut(),
                _file,
                &mut wreq,
                &mut ts64,
                &mut op_ret,
                &mut value,
            );
            let mut writer = unsafe {
                UserSlicePtr::new(
                    &mut (*u_wreq).status as *mut i32 as *mut c_types::c_void,
                    size_of::<i32>(),
                )
                .writer()
                // UserSlicePtr::new(u_wreq.byte_add((size_of::<u64>() + size_of::<i32>()) as isize) as *mut c_types::c_void, size_of::<i32>()).writer()
            };
            writer.write::<i32>(&op_ret).unwrap();
            if ret != 0 && op_ret != 0 {
                let mut writer = unsafe {
                    UserSlicePtr::new(
                        &mut (*u_wreq).value as *mut i32 as *mut c_types::c_void,
                        size_of::<i32>(),
                    )
                    .writer()
                };
                writer.write::<i32>(&value).unwrap();
            }
            Box::into_raw(monitor);
            if ret != 0 {
                return Err(Error::from_kernel_errno(ret));
            }
            return Ok(0);
        }

        if _cmd.cmd == RROS_MONIOC_UNWAIT {
            u_uwreq = _cmd.arg as *mut RrosMonitorUnWaitReq;
            let mut reader = unsafe {
                UserSlicePtr::new(
                    u_uwreq as *mut c_types::c_void,
                    size_of::<RrosMonitorUnWaitReq>(),
                )
                .reader()
            };
            let res = reader.read::<RrosMonitorUnWaitReq>();
            match res {
                Err(_e) => {
                    Box::into_raw(monitor);
                    return Err(Error::EFAULT);
                }
                Ok(ret_uwreq) => {
                    uwreq = ret_uwreq;
                }
            }

            if monitor.type_foo != RROS_MONITOR_EVENT {
                Box::into_raw(monitor);
                return Err(Error::EINVAL);
            }
            return unwait_monitor(&mut uwreq);
        }

        match _cmd.cmd {
            RROS_MONIOC_ENTER => {
                u_uts = _cmd.arg as *mut RrosTimeSpec;
                let mut reader = unsafe {
                    UserSlicePtr::new(u_uts as *mut c_types::c_void, size_of::<RrosTimeSpec>())
                        .reader()
                };
                let res = reader.read::<RrosTimeSpec>();
                match res {
                    Err(_e) => {
                        Box::into_raw(monitor);
                        return Err(Error::EFAULT);
                    }
                    Ok(ret_uts) => {
                        uts = ret_uts;
                    }
                }
                if uts.tv_nsec >= ONE_BILLION {
                    Box::into_raw(monitor);
                    return Err(Error::EINVAL);
                }
                ts64 = Timespec64::new(uts.tv_sec, uts.tv_nsec);
                let gate_core_ref = if let RrosMonitorCore::Gate(gate_core) = &mut monitor.core {
                    gate_core
                } else {
                    panic!("invalid RrosMonitorCore value");
                };
                ret = gate_core_ref.enter_monitor(Some(&mut ts64));
            }
            RROS_MONIOC_TRYENTER => {
                let gate_core_ref = if let RrosMonitorCore::Gate(gate_core) = &mut monitor.core {
                    gate_core
                } else {
                    panic!("invalid RrosMonitorCore valie");
                };
                ret = gate_core_ref.tryenter_monitor();
            }
            RROS_MONIOC_EXIT => {
                let gate_core_ref = if let RrosMonitorCore::Gate(gate_core) = &mut monitor.core {
                    gate_core
                } else {
                    panic!("invalid RrosMonitorCore valie");
                };
                let fundle = monitor.element.deref().borrow().deref().fundle;
                ret = gate_core_ref.exit_monitor(fundle, &mut monitor.state);
            }
            _ => {
                Box::into_raw(monitor);
                ret = Self::ioctl(_this, _file, _cmd);
                return ret;
            }
        }
        Box::into_raw(monitor);
        ret
    }
}
