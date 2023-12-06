use alloc::rc::Rc;

use core::{
    cell::RefCell, 
    cmp::min,
    convert::TryInto,
    fmt,
    mem::size_of,
    ops::Deref,
    ptr::{null, null_mut}, clone
};

use crate::{
    c_types::*,
    sched::{rros_poll_head, rros_schedule},
    factory::{
        CloneData,
        RROS_CLONE_PUBLIC,
        RROS_CLONE_INPUT,
        RROS_CLONE_OUTPUT,
        RrosElement,
        RrosFactory,
        RrosFactoryInside,
        rros_element_name,
        rust_helper_copy_from_user,
    },
    flags::RrosFlag,
    thread::{atomic_read, atomic_set, rros_init_user_element},
    lock::{
        raw_spin_lock_init,
        raw_spin_lock_irqsave, raw_spin_unlock_irqrestore
    },
    file::RrosFileBinding,
    work::*,
    xbuf::wait_event_interruptible,
};

use kernel::{
    bindings,
    io_buffer::{ IoBufferReader, IoBufferWriter },
    irq_work::IrqWork,
    file::File,
    file_operations::{ FileOpener, FileOperations },
    prelude::*,
    premmpt::running_inband,
    str::CStr,
    sync::{Lock, SpinLock, Mutex},
    spinlock_init,
    user_ptr::{ UserSlicePtr, UserSlicePtrReader, UserSlicePtrWriter },
    uidgid::{KgidT, KuidT},
    vmalloc::c_kzalloc,
    workqueue::*,
    from_kernel_result,
};

const FMODE_ATOMIC_POS: u32 = 0x8000;
type loff_t = i64;
// this should be 64
// pub const CONFIG_RROS_NR_PROXIES: usize = 64;
pub const CONFIG_RROS_NR_PROXIES: usize = 16;
const RROS_PROXY_CLONE_FLAGS: i32 = RROS_CLONE_PUBLIC | RROS_CLONE_OUTPUT | RROS_CLONE_INPUT;
pub struct ProxyRing {
    pub bufmem: *mut u8,
    pub fillsz: bindings::atomic_t,
    pub nesting: i32,
    pub bufsz: u32,
    pub rdoff: u32,
    pub wroff: u32,
    pub reserved: u32,
    pub granularity: u32,
    pub oob_wait: RrosFlag,
    pub inband_wait: bindings::wait_queue_head_t,
    pub relay_work: RrosWork,
    pub lock: SpinLock<i32>,
    pub wq: Option<BoxedQueue>,
    pub worker_lock: Arc<SpinLock<i32>>,
}

impl ProxyRing {
    pub fn new() -> Result<Self> {
        Ok(Self {
            bufmem: 0 as *mut u8,
            fillsz: bindings::atomic_t::default(),
            nesting: 0,
            bufsz: 0,
            rdoff: 0,
            wroff: 0,
            reserved: 0,
            granularity: 0,
            oob_wait: RrosFlag::new(),
            inband_wait: bindings::wait_queue_head_t {
                lock: bindings::spinlock_t {
                    _bindgen_opaque_blob: 0,
                },
                head: bindings::list_head {
                    next: null_mut(),
                    prev: null_mut(),
                },
            },
            relay_work: RrosWork::new(),
            lock: unsafe { SpinLock::new(0) },
            wq: None,
            worker_lock: unsafe {
                Arc::try_new(SpinLock::new(0))?
            },
        })
    }
}

// oob_write -> write
pub struct ProxyOut {
    pub ring: ProxyRing,
}

impl ProxyOut {
    pub fn new() -> Result<Self> {
        Ok(Self {
            ring: ProxyRing::new()?,
        })
    }
} 


pub struct ProxyIn {
    pub ring: ProxyRing,
    pub reqsz: bindings::atomic_t,
    pub on_eof: bindings::atomic_t,
    pub on_error: i32,
}

impl ProxyIn {
    pub fn new() -> Result<Self> {
        Ok(Self {
            ring: ProxyRing::new()?,
            reqsz: bindings::atomic_t::default(),
            on_eof: bindings::atomic_t::default(),
            on_error: 0,
        })
    }
}


pub struct RrosProxy {
    filp: *mut bindings::file,
    // filp: File,
    output: ProxyOut,
    input: ProxyIn,
    element: Rc<RefCell<RrosElement>>,
    poll_head: rros_poll_head,
}

impl RrosProxy {
    // pub fn new(fd: u32) -> Result<Self> {
    pub fn new(filp: *mut bindings::file) -> Result<Self> {
        Ok(Self {
            // filp: File::from_fd(fd)?,
            filp,
            output: ProxyOut::new()?,
            input: ProxyIn::new()?,
            element: Rc::try_new(RefCell::new(RrosElement::new()?))?,
            poll_head: rros_poll_head::new(), 
        })
    }
}

#[repr(C)]
pub struct RrosProxyAttrs {
    fd: u32,
    bufsz: u32,
    granularity: u32,
}

impl RrosProxyAttrs {
    fn new() -> Self {
        RrosProxyAttrs {
            fd: 0,
            bufsz: 0,
            granularity: 0
        }
    }

    fn from_ptr(attrs: *mut RrosProxyAttrs) -> Self {
        unsafe {
            Self { fd: (*attrs).fd, bufsz: (*attrs).bufsz, granularity: (*attrs).granularity }
        }
    }
}

extern "C" {
    fn rust_helper_atomic_add(i: i32, v: *mut bindings::atomic_t);
    fn rust_helper_atomic_sub(i: i32, v: *mut bindings::atomic_t);
    fn rust_helper_atomic_sub_return(i: i32, v: *mut bindings::atomic_t) -> i32;
    fn rust_helper_atomic_add_return(i: i32, v: *mut bindings::atomic_t) -> i32;
    fn rust_helper_atomic_cmpxchg(v: *mut bindings::atomic_t, old: i32, new: i32) -> i32;
}

pub fn atomic_add(i: i32, v: *mut bindings::atomic_t) {
    unsafe { rust_helper_atomic_add(i, v); }
}

pub fn atomic_sub(i: i32, v: *mut bindings::atomic_t) {
    unsafe { rust_helper_atomic_sub(i, v); }
}
pub fn atomic_sub_return(i: i32, v: *mut bindings::atomic_t) -> i32 {
    unsafe { rust_helper_atomic_sub_return(i, v) }
}

pub fn atomic_add_return(i: i32, v: *mut bindings::atomic_t) -> i32 {
    unsafe { rust_helper_atomic_add_return(i, v) }
}

pub fn atomic_cmpxchg(v: *mut bindings::atomic_t, old: i32, new: i32) -> i32 {
    unsafe { rust_helper_atomic_cmpxchg(v, old, new) }
}


pub fn proxy_is_readable(proxy: &RrosProxy) -> bool {
    (proxy.element.borrow().deref().clone_flags & RROS_CLONE_INPUT) != 0
}

pub fn proxy_is_writeable(proxy: &RrosProxy) -> bool {
    pr_info!("the proxy clone flags is {}", proxy.element.borrow().deref().clone_flags);
    (proxy.element.borrow().deref().clone_flags & RROS_CLONE_OUTPUT) != 0
}

fn rounddown(x: usize, y: usize) -> usize {
    x - x % y
}

pub fn relay_output(proxy: &mut RrosProxy) -> Result<usize> {
    let ring: &mut ProxyRing = &mut proxy.output.ring;
    let mut rdoff: u32;
    let mut count: u32;
    let mut len: u32;
    let mut n: u32;
    let mut pos: loff_t = 0;
    let mut ppos: *mut loff_t = null_mut();
    let mut ret: isize = 0;
    let filp = proxy.filp;

    let mut wklock = ring.worker_lock.lock();
    count = atomic_read(&mut ring.fillsz as *mut bindings::atomic_t) as u32;
    rdoff = ring.rdoff;
    ppos= 0 as *mut loff_t;
    if (unsafe { (*filp).f_mode } & FMODE_ATOMIC_POS) != 0 {
        unsafe { bindings::mutex_lock(&(*filp).f_pos_lock as *const bindings::mutex as *mut bindings::mutex) };
        ppos = &mut pos as *mut loff_t;
        pos = unsafe { (*filp).f_pos };
    }
    while count > 0 && ret >= 0 {
        len = count;
        loop {
            if rdoff + len > ring.bufsz {
                n = ring.bufsz - rdoff;
            } else {
                n = len;
            }

            if ring.granularity > 0 {
                n = min(n, ring.granularity);
            }

            ret = unsafe { bindings::kernel_write(filp, ring.bufmem.add(rdoff as usize) as *const c_void, n.try_into().unwrap(),  ppos) };
            if ret >= 0 && !ppos.is_null() {
                unsafe { (*filp).f_pos = *ppos };
            }
            len -= n;
            rdoff = (rdoff + n) % ring.bufsz;
            if len > 0 && ret > 0 {
                continue;
            } else {
                break;
            }
        }

        count = atomic_sub_return(count.try_into().unwrap(), &mut ring.fillsz as *mut bindings::atomic_t) as u32;
    }

    if !ppos.is_null() {
        unsafe { bindings::mutex_unlock(&(*filp).f_pos_lock as *const bindings::mutex as *mut bindings::mutex) };
    }
    ring.rdoff = rdoff;
    drop(wklock);

    // if count == 0 {
    //     rros_singal_poll_events(&proxy.poll_head, POLLOUT|POLLWRNORM);
    // }

    if count < ring.bufsz {
        ring.oob_wait.raise();
        unsafe { bindings::__wake_up( &mut ring.inband_wait as *mut bindings::wait_queue_head, bindings::TASK_NORMAL, 1, 0 as *mut c_void); }
    } else {
        unsafe { rros_schedule(); }
    }
    
    Ok(0)
}

pub fn relay_output_work(work: &mut RrosWork) -> i32 {
    let proxy: *mut RrosProxy = kernel::container_of!(work, RrosProxy, output.ring.relay_work) as *mut RrosProxy;

    relay_output(unsafe { &mut (*proxy) });
    0
}

pub fn can_write_buffer(ring: &mut ProxyRing, size: usize) -> bool {
    atomic_read(&mut ring.fillsz as *mut bindings::atomic_t) as u32 + ring.reserved + size as u32 <= ring.bufsz
}

pub fn do_proxy_write(filp: *mut bindings::file, mut u_buf: *const c_char, count: usize) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let ring: &mut ProxyRing = unsafe { &mut (*proxy).output.ring };

    let mut wroff: u32;
    let mut wbytes: u32;
    let mut n: u32;
    let mut rsvd: u32;
    let mut flags: u64;
    let mut ret: isize;

    if count == 0 {
        return 0;
    }

    if count > ring.bufsz as usize {
        return -(bindings::EFBIG as isize);
    }

    if ring.granularity > 1 && count % (ring.granularity as usize) > 0 {
        return -(bindings::EINVAL as isize);
    }

    flags = raw_spin_lock_irqsave();
    if !can_write_buffer(ring, count) {
        ret = -(bindings::EAGAIN as isize);
        raw_spin_unlock_irqrestore(flags);
        return ret;
    } else {
        wroff = ring.wroff;
        ring.wroff = (wroff + count as u32) % ring.bufsz;
        ring.nesting += 1;
        ring.reserved += count as u32;
        ret = count as isize;
        wbytes = count as u32;

        loop {
            if wroff + wbytes > ring.bufsz {
                n = ring.bufsz - wroff;
            } else {
                n = wbytes;
            }

            raw_spin_unlock_irqrestore(flags);

            let uptrrd = u_buf as *mut UserSlicePtrReader;
            let res = unsafe{ (*uptrrd).read_raw((ring.bufmem as usize + wroff as usize) as *mut u8, n as usize) };

            flags = raw_spin_lock_irqsave();
    
            let ret = match res {
                Ok(()) => 0,
                Err(e) => -(bindings::EFAULT as i32),
            };

            u_buf = unsafe { u_buf.add(n.try_into().unwrap()) };
            wbytes -= n;
            wroff = (wroff + n) % ring.bufsz;

            if(wbytes > 0) {
                continue;
            } else {
                break;
            }
        }

        ring.nesting -= 1;
        if ring.nesting == 0 {
            n = atomic_add_return(ring.reserved as i32, &mut ring.fillsz) as u32;
            rsvd = ring.reserved;
            ring.reserved = 0;

            if n == rsvd {
                if running_inband().is_ok() {
                    raw_spin_unlock_irqrestore(flags);
                    relay_output(unsafe { &mut (*proxy) });
                    return ret;
                }
                pr_info!("there has been called");
                ring.relay_work.call_inband_from((ring.wq.as_ref().unwrap().deref().get_ptr()));
            }
        }
        raw_spin_unlock_irqrestore(flags);
        ret
    }
}

pub fn relay_input(proxy: &mut RrosProxy) -> Result<usize> {
    let proxyin: &mut ProxyIn = &mut proxy.input;
    let ring: &mut ProxyRing = &mut proxyin.ring;
    let mut wroff: u32;
    let mut count: u32;
    let mut len: u32;
    let mut n: u32;
    let mut pos: loff_t = 0;
    let mut ppos: *mut loff_t = null_mut();
    let mut ret: isize = 0;
    let mut exception: bool = false;
    let filp: *mut bindings::file = proxy.filp;

    let mut wklock = ring.worker_lock.lock();
    count = atomic_read(&mut proxyin.reqsz as *mut bindings::atomic_t) as u32;
    wroff = ring.wroff;
    ppos= 0 as *mut loff_t;
    if (unsafe { (*filp).f_mode } & FMODE_ATOMIC_POS) != 0 {
        unsafe { bindings::mutex_lock(&(*filp).f_pos_lock as *const bindings::mutex as *mut bindings::mutex) };
        ppos = &mut pos as *mut loff_t;
        pos = unsafe { (*filp).f_pos };
    }

    'outer1: while count > 0 {
        len = count;
        'inner1: loop {
            if wroff + len > ring.bufsz {
                n = ring.bufsz - wroff;
            } else {
                n = len;
            }

            if ring.granularity > 0 {
                n = min(n, ring.granularity);
            }

            ret = unsafe { bindings::kernel_read(filp, ring.bufmem.add(wroff as usize) as *mut c_void, n.try_into().unwrap(),  ppos) };
            if ret <= 0 {
                atomic_sub((count - len) as i32, &mut proxyin.reqsz as *mut bindings::atomic_t);
                if ret !=0 {
                    proxyin.on_error = ret as i32;
                } else {
                    atomic_set(&mut proxyin.on_eof, 1);
                }
                exception = true;
                break 'outer1;
            }
            if !ppos.is_null() {
                unsafe { (*filp).f_pos = *ppos };
            }
            atomic_add(ret as i32, &mut ring.fillsz);
            len -= (ret as u32);
            wroff = (wroff + n) % ring.bufsz;
            if len > 0 {
                continue 'inner1;
            } else {
                break 'inner1;
            }
        }

        count = atomic_sub_return(count.try_into().unwrap(), &mut proxyin.reqsz as *mut bindings::atomic_t) as u32;
    }

    if !ppos.is_null() {
        unsafe { bindings::mutex_unlock(&(*filp).f_pos_lock as *const bindings::mutex as *mut bindings::mutex) };
    }

    ring.wroff = wroff;

    drop(wklock);
    if atomic_read(&mut ring.fillsz as *mut bindings::atomic_t) > 0 || exception {
    //     rros_singal_poll_events(proxy.poll_head, POLLIN|POLLRDNORM);
        ring.oob_wait.raise();
        unsafe { bindings::__wake_up( &mut ring.inband_wait as *mut bindings::wait_queue_head, bindings::TASK_NORMAL, 1, 0 as *mut c_void); }
    }
Ok(0)
}


pub fn relay_input_work(work: &mut RrosWork) -> i32 {
    let proxy: *mut RrosProxy = kernel::container_of!(work, RrosProxy, input.ring.relay_work) as *mut RrosProxy;

    relay_input(unsafe { &mut (*proxy) });
    0
}

pub fn do_proxy_read(filp: *mut bindings::file, mut u_buf: *const c_char, count: usize) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let proxyin: &mut ProxyIn = unsafe { &mut (*proxy).input };
    let ring: &mut ProxyRing = &mut proxyin.ring;
    let mut len: isize;
    let mut ret: isize;
    let mut rbytes: isize;
    let mut n: isize;
    let mut rdoff: u32;
    let mut avail: u32;
    let mut flags: u64;
    let mut u_ptr: *const c_char;
    let xret: i32;

    if count > ring.bufsz as usize {
        return -(bindings::EFBIG as isize);
    }

    if ring.granularity > 1 && (count % ring.granularity as usize) > 0 {
        return -(bindings::EINVAL as isize);
    }

    len = count as isize;
    'outer: loop {
        unsafe { u_ptr = u_buf; }
        'inner: loop {
            flags = raw_spin_lock_irqsave();
            
            avail = atomic_read(&mut ring.fillsz as *mut bindings::atomic_t) as u32 - ring.reserved;
            if avail < len as u32 {
                raw_spin_unlock_irqrestore(flags);
                if avail > 0 && (unsafe { (*filp).f_flags } & bindings::O_NONBLOCK) != 0 {
                    if ring.granularity != 0 {
                        len = rounddown(avail as usize, ring.granularity as usize) as isize;
                    } else {
                        len = rounddown(avail as usize, 1) as isize;
                    }
                
                    if len != 0 {
                        continue 'outer;
                    }
                }
                
                if proxyin.on_error != 0 {
                    return proxyin.on_error as isize;
                }

                return 0;
            }
            rdoff = ring.rdoff;
            ring.rdoff = (rdoff + len as u32) % ring.bufsz;
            ring.nesting += 1;
            ring.reserved += len as u32;
            ret = len;
            rbytes = ret;

            'rbytes: loop {
                if rdoff + rbytes as u32 > ring.bufsz {
                    n = (ring.bufsz - rdoff) as isize;
                } else {
                    n = rbytes;
                }

                raw_spin_unlock_irqrestore(flags);

                let uptrwt = u_buf as *mut UserSlicePtrWriter;
                let res = unsafe{ (*uptrwt).write_raw((ring.bufmem as usize + rdoff as usize) as *const u8, n as usize) };

                flags = raw_spin_lock_irqsave();
    
                let ret = match res {
                    Ok(()) => 0,
                    Err(e) => -(bindings::EFAULT as i32),
                };

                u_buf = unsafe { u_buf.add(n.try_into().unwrap()) };
                rbytes -= n;
                rdoff = (rdoff + n as u32) % ring.bufsz;

                if(rbytes > 0) {
                    continue 'rbytes;
                } else {
                    break 'rbytes;
                }
            }

            ring.nesting -= 1;
            if ring.nesting == 0 {
                atomic_sub(ring.reserved as i32, &mut ring.fillsz as *mut bindings::atomic_t);
                ring.reserved = 0;
            }

            break 'outer;
        }
    }

    raw_spin_unlock_irqrestore(flags);

    unsafe { rros_schedule() ;}

    ret
}

pub fn proxy_oob_write<T: IoBufferReader>(filp: *mut bindings::file, data: &mut T) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let ring: &mut ProxyRing = unsafe {
        &mut (*proxy).output.ring
    };

    let mut ret: isize;

    if !proxy_is_writeable(unsafe { &(*proxy) }) {
        return -(bindings::ENXIO as isize);
    }

    loop {
        ret = do_proxy_write(filp, data as *const _ as *const c_char, data.len());
        if ret != -(bindings::EAGAIN as isize) || unsafe { (*filp).f_flags } & bindings::O_NONBLOCK != 0 {
            break;
        }

        ret = ring.oob_wait.wait() as isize;
        if ret == 0 {
            continue;
        } else {
            break;
        }
    }

    if ret == -(bindings::EIDRM as isize) {
        -(bindings::EBADF as isize)
    } else {
        ret
    }
}

pub fn proxy_oob_read<T: IoBufferWriter>(filp: *mut bindings::file, data: &mut T) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let proxyin: &mut ProxyIn = unsafe {
        &mut (*proxy).input
    };
    let ring: &mut ProxyRing = &mut proxyin.ring;

    let mut request_done: bool = false;
    let mut ret: isize;

    if !proxy_is_readable(unsafe { &(*proxy) }) {
        return -(bindings::ENXIO as isize);
    }

    let count = data.len();
    if count == 0 {
        return 0;
    }

    loop {
        ret = do_proxy_read(filp, data as *const _ as *const c_char, data.len());
        if ret != 0 || unsafe { (*filp).f_flags } & bindings::O_NONBLOCK != 0 {
            break;
        }

        if !request_done {
            atomic_add(count as i32, &mut proxyin.reqsz as *mut bindings::atomic_t);
            request_done = true;
        }

        ring.relay_work.call_inband_from((ring.wq.as_ref().unwrap().deref().get_ptr()));
        ret = ring.oob_wait.wait() as isize;
        if ret != 0 {
            break;
        }
        if atomic_cmpxchg(&mut proxyin.on_eof as *mut bindings::atomic_t, 1, 0) == 1 {
            ret = 0;
            break;
        }
    }

    if ret == -(bindings::EIDRM as isize) {
        -(bindings::EBADF as isize)
    } else {
        ret
    }
}

// static __poll_t proxy_oob_poll(struct file *filp,
//     struct oob_poll_wait *wait)
// {
// struct rros_proxy *proxy = element_of(filp, struct rros_proxy);
// struct proxy_ring *oring = &proxy->output.ring;
// struct proxy_ring *iring = &proxy->input.ring;
// __poll_t ret = 0;
// int peek;

// if (!(proxy_is_readable(proxy) || proxy_is_writable(proxy)))
// return POLLERR;

// rros_poll_watch(&proxy->poll_head, wait, NULL);

// if (proxy_is_writable(proxy) &&
// atomic_read(&oring->fillsz) < oring->bufsz)
// ret = POLLOUT|POLLWRNORM;

// /*
// * If the input ring is empty, kick the worker to perform a
// * readahead as a last resort.
// */
// if (proxy_is_readable(proxy)) {
// if (atomic_read(&iring->fillsz) > 0)
//     ret |= POLLIN|POLLRDNORM;
// else if (atomic_read(&proxy->input.reqsz) == 0) {
//     peek = iring->granularity ?: 1;
//     atomic_add(peek, &proxy->input.reqsz);
//     rros_call_inband_from(&iring->relay_work, iring->wq);
// }
// }

// return ret;
// }

pub fn proxy_write<T: IoBufferReader>(filp: *mut bindings::file, data: &mut T) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let ring: &mut ProxyRing = unsafe { &mut (*proxy).output.ring };
    let mut ret: isize;

    if !proxy_is_writeable(unsafe { &(*proxy) }) {
        return -(bindings::ENXIO as isize);
    }

    loop {
        ret = do_proxy_write(filp, data as *const _ as *const c_char, data.len());
        if ret != 0 || unsafe { (*filp).f_flags } & bindings::O_NONBLOCK != 0 {
            break;
        }

        wait_event_interruptible(&ring.inband_wait as *const _ as *mut bindings::wait_queue_head, can_write_buffer(ring, data.len()));

        if ret == 0 {
            continue;
        } else {
            break;
        }
    }

    ret
}

pub fn proxy_read<T: IoBufferWriter>(filp: *mut bindings::file, data: &mut T) -> isize {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let proxy = unsafe { (*((*fbind).element)).pointer as *mut RrosProxy};
    let proxyin: &mut ProxyIn = unsafe {
        &mut (*proxy).input
    };

    let mut request_done: bool = false;
    let mut ret: isize;

    if !proxy_is_readable(unsafe { &(*proxy) }) {
        return -(bindings::ENXIO as isize);
    }

    let count = data.len();
    if count == 0 {
        return 0;
    }

    loop {
        ret = do_proxy_read(filp, data as *mut _ as *const c_char, data.len());
        if ret != 0 || unsafe { (*filp).f_flags } & bindings::O_NONBLOCK != 0 {
            break;
        }

        if !request_done {
            atomic_add(count as i32, &mut proxyin.reqsz as *mut bindings::atomic_t);
            request_done = true;
        }

        relay_input(unsafe { &mut (*proxy) });
        if atomic_cmpxchg(&mut proxyin.on_eof as *mut bindings::atomic_t, 1, 0) == 1 {
            ret = 0;
            break;
        }
    }

    ret
}

// static __poll_t proxy_poll(struct file *filp, poll_table *wait)
// {
// 	struct rros_proxy *proxy = element_of(filp, struct rros_proxy);
// 	struct proxy_ring *oring = &proxy->output.ring;
// 	struct proxy_ring *iring = &proxy->input.ring;
// 	__poll_t ret = 0;

// 	if (!(proxy_is_readable(proxy) || proxy_is_writable(proxy)))
// 		return POLLERR;

// 	if (proxy_is_writable(proxy)) {
// 		poll_wait(filp, &oring->inband_wait, wait);
// 		if (atomic_read(&oring->fillsz) < oring->bufsz)
// 			ret = POLLOUT|POLLWRNORM;
// 	}

// 	if (proxy_is_readable(proxy)) {
// 		poll_wait(filp, &iring->inband_wait, wait);
// 		if (atomic_read(&iring->fillsz) > 0)
// 			ret |= POLLIN|POLLRDNORM;
// 		else if (proxy->filp->f_op->poll) {
// 			ret = proxy->filp->f_op->poll(proxy->filp, wait);
// 			ret &= POLLIN|POLLRDNORM;
// 		}
// 	}

// 	return ret;
// }

// static int proxy_mmap(struct file *filp, struct vm_area_struct *vma)
// {
// 	struct rros_proxy *proxy = element_of(filp, struct rros_proxy);
// 	struct file *mapfilp = proxy->filp;
// 	int ret;

// 	if (mapfilp->f_op->mmap == NULL)
// 		return -ENODEV;

// 	vma->vm_file = get_file(mapfilp);

// 	/*
// 	 * Since the mapper element impersonates a different file, we
// 	 * need to swap references: if the mapping call fails, we have
// 	 * to drop the reference on the target file we just took on
// 	 * entry; if it succeeds, then we have to drop the reference
// 	 * on the mapper file do_mmap_pgoff() acquired before calling
// 	 * us.
// 	 */
// 	ret = call_mmap(mapfilp, vma);
// 	if (ret)
// 		fput(mapfilp);
// 	else
// 		fput(filp);

// 	return ret;
// }

pub fn init_output_ring(proxy: &mut RrosProxy, bufsz: u32, granularity: u32) -> Result<usize> {
    let ring = &mut proxy.output.ring;
    let bufmem = c_kzalloc(bufsz as u64);
    let wq :BoxedQueue = Queue::try_new(format_args!("{}", unsafe { *(rros_element_name(proxy.element.borrow_mut().deref())) }))?;

    ring.wq = Some(wq);
	ring.bufmem = bufmem.unwrap() as *mut u8;
	ring.bufsz = bufsz;
	ring.granularity = granularity;
	unsafe { raw_spin_lock_init(&mut ring.lock) };
    ring.relay_work.init_safe(relay_output_work, proxy.element.clone());
	ring.oob_wait.init();
    let key = bindings::lock_class_key::default();
    let name = unsafe { CStr::from_bytes_with_nul_unchecked("PROXY RING INBAND WAITQUEUE HEAD\0".as_bytes()) };
    unsafe { bindings::__init_waitqueue_head(&ring.inband_wait as *const _ as *mut bindings::wait_queue_head, name.as_ptr() as *const i8, &key as *const _ as *mut bindings::lock_class_key) };
	unsafe { raw_spin_lock_init(Arc::get_mut_unchecked(&mut ring.worker_lock.clone())) };

    Ok(0)
}

pub fn init_input_ring(proxy: &mut RrosProxy, bufsz: u32, granularity: u32) -> Result<usize> {
    let ring = &mut proxy.input.ring;
    let bufmem = c_kzalloc(bufsz as u64);
    let wq :BoxedQueue = Queue::try_new(format_args!("{}", unsafe { *(rros_element_name(proxy.element.borrow_mut().deref())) }))?;

    ring.wq = Some(wq);
	ring.bufmem = bufmem.unwrap() as *mut u8;
	ring.bufsz = bufsz;
	ring.granularity = granularity;
	unsafe { raw_spin_lock_init(&mut ring.lock) };
    ring.relay_work.init_safe(relay_input_work, proxy.element.clone());
	ring.oob_wait.init();
    let key = bindings::lock_class_key::default();
    let name = unsafe { CStr::from_bytes_with_nul_unchecked("PROXY RING INBAND WAITQUEUE HEAD\0".as_bytes()) };
    unsafe { bindings::__init_waitqueue_head(&ring.inband_wait as *const _ as *mut bindings::wait_queue_head, name.as_ptr() as *const i8, &key as *const _ as *mut bindings::lock_class_key) };
	unsafe { raw_spin_lock_init(Arc::get_mut_unchecked(&mut ring.worker_lock.clone())) };

    Ok(0)
}

fn proxy_factory_build(fac: &'static mut SpinLock<RrosFactory>, uname: &'static CStr, u_attrs: Option<*mut u8>, mut clone_flags: i32, state_offp: &u32) -> Rc<RefCell<RrosElement>> {
    pr_info!("clone_flags = {}", clone_flags);
    if clone_flags & !RROS_PROXY_CLONE_FLAGS != 0 {
        pr_info!("invalid proxy clone flags");
    }
    pr_info!("the u_attrs: {:p}", u_attrs.unwrap());

    let attrs = RrosProxyAttrs::from_ptr(u_attrs.unwrap() as *mut RrosProxyAttrs);
    pr_info!("the attrs.fd is {}", attrs.fd);
    let bufsz = attrs.bufsz;

    if bufsz == 0 && (clone_flags & (RROS_CLONE_INPUT|RROS_CLONE_OUTPUT) != 0) {
        pr_info!("invalid proxy bufsz value");
    }

    //If a granularity is set, the buffer size must be a multiple of the granule size. 
    if attrs.granularity > 1 && bufsz % attrs.granularity > 0 {
        pr_info!("invalid granularity value");
    }

    pr_info!("clone_flags = {}", clone_flags);
	if (bufsz > 0 && (clone_flags & (RROS_CLONE_INPUT|RROS_CLONE_OUTPUT) == 0)) {
		clone_flags |= RROS_CLONE_OUTPUT;
    }
    pr_info!("clone_flags = {}", clone_flags);

    let mut boxed_proxy = Box::try_new(RrosProxy::new(unsafe { bindings::fget(attrs.fd) }).unwrap()).unwrap();
    let proxy = Box::into_raw(boxed_proxy);
    unsafe {
        let ret = rros_init_user_element(unsafe { (*proxy).element.clone() }, fac, uname, clone_flags);
        if let Err(_e) = ret {
            pr_info!("init user element failed");
        }

        // TODO: rros_init_poll_head()
    pr_info!("clone_flags = {}", clone_flags);
	    if (clone_flags & RROS_CLONE_OUTPUT) != 0 {
            let res = init_output_ring(&mut (*proxy), bufsz, attrs.granularity);

            if let Err(_e) = res{
                pr_info!("init proxy output ring failed");
            }
        }
	    if (clone_flags & RROS_CLONE_INPUT) != 0 {
            let res = init_input_ring(&mut (*proxy), bufsz, attrs.granularity);

            if let Err(_e) = res {
                pr_info!("init proxy input ring failed");
            }
        }
    }
    // TODO: rros_index_factory_element();
    unsafe { (*(*proxy).element.borrow_mut()).pointer = proxy as *mut u8; }
    unsafe { (*proxy).element.clone() }
}

pub static mut RROS_PROXY_FACTORY: SpinLock<RrosFactory> = unsafe {
    SpinLock::new(RrosFactory {
        name: unsafe { CStr::from_bytes_with_nul_unchecked("RROS_PROXY_DEV\0".as_bytes()) },
        // fops: Some(&RustFileProxy),
        nrdev: CONFIG_RROS_NR_PROXIES,
        build: Some(proxy_factory_build),
        dispose: Some(proxy_factory_dispose),
        attrs: None, //sysfs::attribute_group::new(),
        flags: 1,
        inside: Some(RrosFactoryInside {
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
pub struct ProxyOps;

impl FileOpener<u8> for ProxyOps {
    fn open(shared: &u8, fileref: &File) -> Result<Self::Wrapper> {
        let mut data = CloneData::default();
        unsafe{ data.ptr = shared as *const u8 as *mut u8; }
        pr_info!("open proxy device success");
        Ok(Box::try_new(data)?)
    }
}

impl FileOperations for ProxyOps{
    kernel::declare_file_operations!(read, write, oob_read, oob_write);

    type Wrapper = Box<CloneData>;

    fn read<T: IoBufferWriter>(
        _this: &CloneData,
        file: &File,
        data: &mut T,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("I'm the read ops of the proxy factory.");
        let ret = proxy_read(file.get_ptr(), data);
        pr_info!("the result of proxy read is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret.try_into().unwrap()))
        } else {
            Ok(ret as usize)
        }
    }

    fn oob_read<T: IoBufferWriter>(
        _this: &CloneData,
        file: &File,
        data: &mut T,
    ) -> Result<usize> {
        pr_info!("I'm the oob_read ops of the proxy factory.");
        let ret = proxy_oob_read(file.get_ptr(), data);
        pr_info!("the result of proxy oob_read is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret.try_into().unwrap()))
        } else {
            Ok(ret as usize)
        }
    }

    fn write<T: IoBufferReader>(
        _this: &CloneData,
        file: &File,
        data: &mut T,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("I'm the write ops of the proxy factory.");
        let ret = proxy_write(file.get_ptr(), data);
        pr_info!("the result of proxy write is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret.try_into().unwrap()))
        } else {
            Ok(ret as usize)
        }
    }

    fn oob_write<T: IoBufferReader>(
        _this: &CloneData,
        file: &File,
        data: &mut T,
    ) -> Result<usize> {
        pr_info!("I'm the oob_write ops of the proxy factory.");
        let ret = proxy_oob_write(file.get_ptr(), data);
        pr_info!("the result of proxy oob_write is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret.try_into().unwrap()))
        } else {
            Ok(ret as usize)
        }
    }

    fn release(
        _this: Box<CloneData>,
        _file: &File,
    ) {
        pr_info!("I'm the release ops from the proxy ops.");
        // FIXME: put the rros element
    }
}

pub fn proxy_factory_dispose(ele: RrosElement) {}