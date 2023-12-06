use alloc::rc::Rc;

use core::{
    cell::RefCell,
    convert::TryFrom,
    mem::size_of,
    sync::atomic::{AtomicUsize, Ordering},
};

use crate::{
    clock, factory,
    factory::{rros_init_element, RrosElement, RrosFactory},
    fifo::RROS_FIFO_MAX_PRIO,
    sched,
    thread::atomic_set,
    Box, wait::RrosWaitQueue,
};

use kernel::{
    bindings, c_types, prelude::*, spinlock_init, str::CStr, sync::SpinLock, user_ptr, Error,
};

use kernel::file::File;
use kernel::file_operations::FileOperations;
use kernel::io_buffer::IoBufferWriter;

pub struct RrosMonitorItem1 {
    pub mutex: SpinLock<i32>,
    pub events: sched::list_head,
    pub lock: SpinLock<i32>,
}

impl RrosMonitorItem1 {
    fn new() -> Result<Self> {
        Ok(Self {
            mutex: unsafe { SpinLock::new(0) },
            events: sched::list_head::new(),
            lock: unsafe { SpinLock::<i32>::new(0) },
        })
    }
}

pub struct RrosMonitorItem2 {
    pub wait_queue: RrosWaitQueue,
    pub gate: Option<*mut u8>,
    pub poll_head: sched::rros_poll_head,
    pub next: sched::list_head,
    pub next_poll: sched::list_head,
}

impl RrosMonitorItem2 {
    fn new() -> Result<Self> {
        Ok(Self {
            wait_queue: unsafe{core::mem::zeroed()},
            gate: None,
            poll_head: sched::rros_poll_head::new(),
            next: sched::list_head::new(),
            next_poll: sched::list_head::new(),
        })
    }
}

pub enum RrosMonitorItem {
    Item1(RrosMonitorItem1),
    Item2(RrosMonitorItem2),
}

pub struct RrosMonitor {
    pub element: Rc<RefCell<RrosElement>>,
    pub state: Option<RrosMonitorState>,
    pub type_foo: i32,
    pub protocol: i32,
    pub item: RrosMonitorItem,
}

impl RrosMonitor {
    pub fn new(
        element: Rc<RefCell<RrosElement>>,
        state: Option<RrosMonitorState>,
        type_foo: i32,
        protocol: i32,
        item: RrosMonitorItem,
    ) -> Result<Self> {
        match item {
            RrosMonitorItem::Item1(subitem) => Ok(Self {
                element,
                state,
                type_foo,
                protocol,
                item: RrosMonitorItem::Item1(subitem),
            }),
            RrosMonitorItem::Item2(subitem) => Ok(Self {
                element,
                state,
                type_foo,
                protocol,
                item: RrosMonitorItem::Item2(subitem),
            }),
        }
    }
}

// #[derive(Copy, Clone)]
pub struct RrosMonitorStateItemGate {
    owner: AtomicUsize,
    ceiling: u32,
    recursive: u32,
    nesting: u32,
}

// #[derive(Copy, Clone)]
pub struct RrosMonitorStateItemEvent {
    value: AtomicUsize,
    pollrefs: AtomicUsize,
    gate_offset: u32,
}

// union RrosMonitorState_item {
//     gate: RrosMonitorState_item_gate,
//     event: RrosMonitorState_item_event,
// }
pub enum RrosMonitorStateItem {
    Gate(RrosMonitorStateItemGate),
    Event(RrosMonitorStateItemEvent),
}

pub struct RrosMonitorState {
    pub flags: u32,
    pub u: Option<RrosMonitorStateItem>,
}

impl RrosMonitorState {
    pub fn new() -> Result<Self> {
        Ok(Self { flags: 0, u: None })
    }
}

pub struct RrosMonitorAttrs {
    clockfd: u32,
    type_foo: u32,
    protocol: u32,
    initval: u32,
}

impl RrosMonitorAttrs {
    fn new() -> Result<Self> {
        Ok(Self {
            clockfd: 0,
            type_foo: 0,
            protocol: 0,
            initval: 0,
        })
    }
}

pub const RROS_MONITOR_EVENT: u32 = 0; /* Event monitor. */
pub const RROS_EVENT_GATED: u32 = 0; /* Gate protected. */
pub const RROS_EVENT_COUNT: u32 = 1; /* Semaphore. */
pub const RROS_EVENT_MASK: u32 = 2; /* Event (bit)mask. */
pub const RROS_MONITOR_GATE: u32 = 1; /* Gate monitor. */
pub const RROS_GATE_PI: u32 = 0; /* Gate with priority inheritance. */
pub const RROS_GATE_PP: u32 = 1; /* Gate with priority protection (ceiling). */

pub const RROS_MONITOR_NOGATE: u32 = 1;
pub const CLOCK_MONOTONIC: u32 = 1;
pub const CLOCK_REALTIME: u32 = 0;

const CONFIG_RROS_MONITOR: usize = 0; //未知

pub fn monitor_factory_build(
    fac: *mut RrosFactory,
    uname: &'static CStr,
    u_attrs: Option<*mut u8>,
    clone_flags: i32,
    state_offp: &u32,
) -> Result<Rc<RefCell<RrosElement>>> {
    if (clone_flags & !factory::RROS_CLONE_PUBLIC) != 0 {
        return Err(Error::EINVAL);
    }

    let mut attrs = RrosMonitorAttrs::new()?;
    let len = size_of::<RrosMonitorAttrs>();
    let ptr: *mut c_types::c_void = &mut attrs as *mut RrosMonitorAttrs as *mut c_types::c_void;
    let u_attrs: *const c_types::c_void =
        &attrs as *const RrosMonitorAttrs as *const c_types::c_void;
    let ret = unsafe { user_ptr::rust_helper_copy_from_user(ptr, u_attrs as _, len as _) };
    if ret != 0 {
        return Err(Error::EFAULT);
    }

    match attrs.type_foo {
        RROS_MONITOR_GATE => match attrs.protocol {
            RROS_GATE_PP => {
                if attrs.initval == 0 || attrs.initval > RROS_FIFO_MAX_PRIO as u32 {
                    return Err(Error::EINVAL);
                }
            }
            RROS_GATE_PI => {
                if attrs.initval != 0 {
                    return Err(Error::EINVAL);
                }
            }
            _ => return Err(Error::EINVAL),
        },
        RROS_MONITOR_EVENT => match attrs.protocol {
            RROS_EVENT_GATED | RROS_EVENT_COUNT | RROS_EVENT_MASK => (),
            _ => return Err(Error::EINVAL),
        },
        _ => return Err(Error::EINVAL),
    }

    let clock: Result<&mut clock::RrosClock> = {
        match attrs.clockfd {
            CLOCK_MONOTONIC => unsafe { Ok(&mut clock::RROS_MONO_CLOCK) },
            _ => unsafe { Ok(&mut clock::RROS_REALTIME_CLOCK) },
        }
    };

    let element = Rc::try_new(RefCell::new(RrosElement::new()?))?;
    let mut factory: &mut SpinLock<RrosFactory> = unsafe { &mut RROS_MONITOR_FACTORY };
    let ret = factory::rros_init_element(element.clone(), factory, clone_flags);

    let mut state = RrosMonitorState::new()?;

    match attrs.type_foo {
        RROS_MONITOR_GATE => match attrs.protocol {
            RROS_GATE_PP => {
                state.u = Some(RrosMonitorStateItem::Gate(RrosMonitorStateItemGate {
                    owner: AtomicUsize::new(0),
                    ceiling: attrs.initval,
                    recursive: 0,
                    nesting: 0,
                }));
            }
            RROS_GATE_PI => {
                ();
            }
            _ => (),
        },
        RROS_MONITOR_EVENT => {
            state.u = Some(RrosMonitorStateItem::Event(RrosMonitorStateItemEvent {
                value: AtomicUsize::new(usize::try_from(attrs.initval)?),
                pollrefs: AtomicUsize::new(0),
                gate_offset: RROS_MONITOR_NOGATE,
            }));
        }
        _ => (),
    }

    // init monitor
    let mon = match state.u {
        Some(RrosMonitorStateItem::Gate(ref RrosMonitorStateItemGate)) => {
            let mut item = RrosMonitorItem1::new()?;
            let pinned = unsafe { Pin::new_unchecked(&mut item.mutex) };
            spinlock_init!(pinned, "RrosMonitorItem1_lock");

            let pinned = unsafe { Pin::new_unchecked(&mut item.lock) };
            spinlock_init!(pinned, "value");
            RrosMonitor::new(
                element,
                Some(state),
                attrs.type_foo as i32,
                attrs.protocol as i32,
                RrosMonitorItem::Item1(item),
            )?
        }
        _ => {
            let item = RrosMonitorItem2::new()?;
            RrosMonitor::new(
                element,
                Some(state),
                attrs.type_foo as i32,
                attrs.protocol as i32,
                RrosMonitorItem::Item2(item),
            )?
        }
    };

    // *state_offp = rros_shared_offset(state) // todo
    // rros_index_factory_element(&mon->element)

    return Ok(mon.element);
}

pub static mut RROS_MONITOR_FACTORY: SpinLock<factory::RrosFactory> = unsafe {
    SpinLock::new(factory::RrosFactory {
        name: unsafe { CStr::from_bytes_with_nul_unchecked("RROS_MONITOR_DEV\0".as_bytes()) },
        // fops: Some(&MonitorOps),
        nrdev: CONFIG_RROS_MONITOR,
        build: None,
        dispose: Some(monitor_factory_dispose),
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

pub fn monitor_factory_dispose(ele: factory::RrosElement) {}

struct MonitorOps;

impl FileOperations for MonitorOps {
    kernel::declare_file_operations!(read);

    fn read<T: IoBufferWriter>(
        _this: &Self,
        _file: &File,
        _data: &mut T,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("I'm the read ops of the rros monitor factory.");
        Ok(1)
    }
}