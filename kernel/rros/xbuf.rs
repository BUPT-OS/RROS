use alloc::rc::Rc;

use core::{cell::RefCell, convert::TryInto, ptr::NonNull};

use crate::{
    clock::{RrosClock, RROS_MONO_CLOCK},
    factory::*,
    file::*,
    flags::RrosFlag,
    lock::*,
    sched::*,
    thread::rros_init_user_element,
    timeout::{RrosTmode, RROS_INFINITE},
    wait::{RrosWaitQueue, RROS_WAIT_PRIO},
};

use kernel::{
    bindings,
    c_types::*,
    error::Error,
    file::File,
    file_operations::{FileOpener, FileOperations},
    io_buffer::{IoBufferReader, IoBufferWriter},
    irq_work::*,
    prelude::*,
    str::CStr,
    sync::SpinLock,
    user_ptr::{UserSlicePtrReader, UserSlicePtrWriter},
    vmalloc::c_kzalloc,
};

#[derive(Default)]
pub struct XbufOps;

impl FileOpener<u8> for XbufOps {
    fn open(shared: &u8, _fileref: &File) -> Result<Self::Wrapper> {
        let mut data = CloneData::default();
        data.ptr = shared as *const u8 as *mut u8;
        pr_info!("open xbuf device success");
        Ok(Box::try_new(data)?)
    }
}

impl FileOperations for XbufOps {
    kernel::declare_file_operations!(read, write, oob_read, oob_write);

    type Wrapper = Box<CloneData>;

    fn read<T: IoBufferWriter>(
        _this: &CloneData,
        file: &File,
        data: &mut T,
        _offset: u64,
    ) -> Result<usize> {
        pr_debug!("I'm the read ops of the xbuf factory.");
        let ret = xbuf_read(file.get_ptr(), data);
        pr_debug!("the result of xbuf read is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret))
        } else {
            Ok(ret as usize)
        }
    }

    fn oob_read<T: IoBufferWriter>(_this: &CloneData, file: &File, data: &mut T) -> Result<usize> {
        pr_debug!("I'm the oob_read ops of the xbuf factory.");
        let ret = xbuf_oob_read(file.get_ptr(), data);
        pr_debug!("the result of xbuf oob_read is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret))
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
        pr_debug!("I'm the write ops of the xbuf factory.");
        let ret = xbuf_write(file.get_ptr(), data);
        pr_debug!("the result of xbuf write is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret))
        } else {
            Ok(ret as usize)
        }
    }

    fn oob_write<T: IoBufferReader>(_this: &CloneData, file: &File, data: &mut T) -> Result<usize> {
        pr_debug!("I'm the oob_write ops of the xbuf factory.");
        let ret = xbuf_oob_write(file.get_ptr(), data);
        pr_debug!("the result of xbuf oob_write is {}", ret);
        if ret < 0 {
            Err(Error::from_kernel_errno(ret))
        } else {
            Ok(ret as usize)
        }
    }

    fn release(_this: Box<CloneData>, _file: &File) {
        pr_debug!("I'm the release ops from the xbuf ops.");
        // FIXME: put the rros element
    }
}

pub const CONFIG_RROS_NR_XBUFS: usize = 16;

pub struct XbufRing {
    pub bufmem: *mut u8,
    pub bufsz: usize,
    pub fillsz: usize,
    pub rdoff: u32,
    pub rdrsvd: u32,
    pub rdpending: i32,
    pub wroff: u32,
    pub wrrsvd: u32,
    pub wrpending: i32,
    pub lock: Option<fn(&XbufRing) -> u32>,
    pub unlock: Option<fn(&XbufRing, flags: u32)>,
    pub wait_input: Option<fn(&XbufRing, len: usize, avail: usize) -> i32>,
    pub signal_input: Option<fn(&XbufRing, sigpoll: bool)>,
    pub wait_output: Option<fn(&XbufRing, len: usize) -> i32>,
    pub signal_output: Option<fn(&XbufRing, sigpoll: bool)>,
}

impl XbufRing {
    pub fn new() -> Result<Self> {
        Ok(Self {
            bufmem: 0 as *mut u8,
            bufsz: 0,
            fillsz: 0,
            rdoff: 0,
            rdrsvd: 0,
            rdpending: 0,
            wroff: 0,
            wrrsvd: 0,
            wrpending: 0,
            lock: None,
            unlock: None,
            wait_input: None,
            signal_input: None,
            wait_output: None,
            signal_output: None,
        })
    }

    fn lock(&self) -> u32 {
        if self.lock.is_some() {
            return self.lock.unwrap()(&self);
        }
        0
    }

    fn unlock(&self, flags: u32) {
        if self.unlock.is_some() {
            self.unlock.unwrap()(&self, flags);
        }
    }

    fn wait_input(&self, len: usize, avail: usize) -> i32 {
        if self.wait_input.is_some() {
            return self.wait_input.unwrap()(&self, len, avail);
        }
        0
    }

    fn signal_input(&self, sigpoll: bool) {
        if self.signal_input.is_some() {
            self.signal_input.unwrap()(&self, sigpoll);
        }
    }

    fn wait_output(&self, len: usize) -> i32 {
        if self.wait_output.is_some() {
            return self.wait_output.unwrap()(&self, len);
        }
        0
    }

    fn signal_output(&self, sigpoll: bool) {
        if self.signal_output.is_some() {
            self.signal_output.unwrap()(&self, sigpoll);
        }
    }
}

// oob_write -> read
pub struct XbufInbound {
    pub i_event: bindings::wait_queue_head,
    pub o_event: RrosFlag,
    pub irq_work: IrqWork,
    pub ring: XbufRing,
    pub lock: SpinLock<i32>,
}

impl XbufInbound {
    pub fn new() -> Result<Self> {
        Ok(Self {
            i_event: bindings::wait_queue_head::default(),
            o_event: RrosFlag::new(),
            irq_work: IrqWork::new(),
            ring: XbufRing::new()?,
            lock: unsafe { SpinLock::new(0) },
        })
    }
}

// write -> oob_read
pub struct XbufOutbound {
    pub i_event: RrosWaitQueue,
    pub o_event: bindings::wait_queue_head,
    pub irq_work: IrqWork,
    pub ring: XbufRing,
}

impl XbufOutbound {
    pub fn new() -> Result<Self> {
        Ok(Self {
            i_event: unsafe {
                RrosWaitQueue::new(
                    &mut RROS_MONO_CLOCK as *mut RrosClock,
                    RROS_WAIT_PRIO as i32,
                )
            },
            o_event: bindings::wait_queue_head::default(),
            irq_work: IrqWork::new(),
            ring: XbufRing::new()?,
        })
    }
}

#[repr(C)]
pub struct RrosXbufAttrs {
    pub i_bufsz: u32,
    pub o_bufsz: u32,
}

impl RrosXbufAttrs {
    #[allow(dead_code)]
    fn new() -> Self {
        RrosXbufAttrs {
            i_bufsz: 0,
            o_bufsz: 0,
        }
    }

    fn from_ptr(attrs: *mut RrosXbufAttrs) -> Self {
        unsafe {
            Self {
                i_bufsz: (*attrs).i_bufsz,
                o_bufsz: (*attrs).o_bufsz,
            }
        }
    }
}

pub struct RrosXbuf {
    pub element: Rc<RefCell<RrosElement>>,
    pub ibnd: XbufInbound,
    pub obnd: XbufOutbound,
    pub poll_head: RrosPollHead,
}

impl RrosXbuf {
    pub fn new() -> Result<Self> {
        Ok(Self {
            element: Rc::try_new(RefCell::new(RrosElement::new()?))?,
            ibnd: XbufInbound::new()?,
            obnd: XbufOutbound::new()?,
            poll_head: RrosPollHead::new(),
        })
    }
}

pub struct XbufRdesc {
    pub buf: *mut c_char,
    pub buf_ptr: *mut c_char,
    pub count: usize,
    pub xfer: Option<fn(dst: &mut XbufRdesc, src: *mut c_char, len: usize) -> i32>,
}

impl XbufRdesc {
    #[allow(dead_code)]
    pub fn new() -> Result<Self> {
        Ok(Self {
            buf: 0 as *mut c_char,
            buf_ptr: 0 as *mut c_char,
            count: 0,
            xfer: None,
        })
    }

    fn xfer(&mut self, src: *mut c_char, len: usize) -> i32 {
        if self.xfer.is_some() {
            return self.xfer.unwrap()(self, src, len);
        }
        0
    }
}

pub fn write_to_user(rd: &mut XbufRdesc, src: *mut c_char, len: usize) -> i32 {
    let uptrwt = rd.buf_ptr as *mut UserSlicePtrWriter;
    let res = unsafe { (*uptrwt).write_raw(src as *mut u8, len) };

    let ret = match res {
        Ok(()) => 0,
        Err(_e) => -(bindings::EFAULT as i32),
    };

    ret
}

#[allow(dead_code)]
pub fn write_to_kernel(rd: &mut XbufRdesc, src: *mut c_char, len: usize) -> i32 {
    unsafe {
        bindings::memcpy(
            rd.buf_ptr as *mut c_void,
            src as *const c_void,
            len as c_ulong,
        );
    }
    0
}

pub struct XbufWdesc {
    pub buf: *const c_char,
    pub buf_ptr: *const c_char,
    pub count: usize,
    pub xfer: Option<fn(src: &mut XbufWdesc, dst: *mut c_char, len: usize) -> i32>,
}

impl XbufWdesc {
    #[allow(dead_code)]
    pub fn new() -> Result<Self> {
        Ok(Self {
            buf: 0 as *const c_char,
            buf_ptr: 0 as *const c_char,
            count: 0,
            xfer: None,
        })
    }

    fn xfer(&mut self, dst: *mut c_char, len: usize) -> i32 {
        if self.xfer.is_some() {
            return self.xfer.unwrap()(self, dst, len);
        }
        0
    }
}

pub fn read_from_user(wd: &mut XbufWdesc, dst: *mut c_char, len: usize) -> i32 {
    let uptrrd = wd.buf_ptr as *mut UserSlicePtrReader;
    let res = unsafe { (*uptrrd).read_raw(dst as *mut u8, len) };

    let ret = match res {
        Ok(()) => 0,
        Err(_e) => -(bindings::EFAULT as i32),
    };

    ret
}

#[allow(dead_code)]
pub fn read_from_kernel(wd: &mut XbufWdesc, dst: *mut c_char, len: usize) -> i32 {
    unsafe {
        bindings::memcpy(
            dst as *mut c_void,
            wd.buf_ptr as *const c_void,
            len as c_ulong,
        );
    }
    0
}

pub fn do_xbuf_read(ring: &mut XbufRing, rd: &mut XbufRdesc, f_flags: i32) -> i32 {
    let sigpoll: bool;
    let mut flags: u32;
    let mut avail: u32;
    let mut ret: i32;
    let mut rbytes: i32;
    let mut n: i32;
    let mut rdoff: u32;
    let mut xret: i32;
    let mut len: u32 = rd.count.try_into().unwrap();

    if len == 0 {
        return 0;
    }

    if ring.bufsz == 0 {
        return -(bindings::ENOBUFS as i32);
    }

    'outer: loop {
        rd.buf_ptr = rd.buf;
        'inner: loop {
            flags = ring.lock();
            avail = (ring.fillsz - ring.rdrsvd as usize).try_into().unwrap();
            if avail < len {
                if (f_flags & bindings::O_NONBLOCK as i32) != 0 {
                    if avail == 0 {
                        ret = -(bindings::EAGAIN as i32);
                        break 'outer;
                    }
                    len = avail;
                } else {
                    if len > ring.bufsz.try_into().unwrap() {
                        ret = -(bindings::EINVAL as i32);
                        break 'outer;
                    }
                    ring.unlock(flags);
                    ret = ring.wait_input(len.try_into().unwrap(), avail.try_into().unwrap());
                    if ret != 0 {
                        if ret == -(bindings::EAGAIN as i32) {
                            len = avail;
                            continue 'outer;
                        }
                        return ret;
                    }
                    continue 'inner;
                }
            }

            rdoff = ring.rdoff;
            ring.rdoff = (rdoff + len) % ring.bufsz as u32;
            ring.rdpending += 1;
            ring.rdrsvd += len;
            ret = len.try_into().unwrap();
            rbytes = ret;

            'rbytes: loop {
                if rdoff as i32 + rbytes > ring.bufsz.try_into().unwrap() {
                    n = ring.bufsz as i32 - rdoff as i32;
                } else {
                    n = rbytes;
                }

                ring.unlock(flags);

                xret = rd.xfer(
                    (ring.bufmem as usize + rdoff as usize) as *mut c_char,
                    n.try_into().unwrap(),
                );
                flags = ring.lock();
                if xret != 0 {
                    ret = -(bindings::EFAULT as i32);
                    break 'rbytes;
                }

                // unsafe { (rd.buf_ptr as i32 += n) as *mut c_char };
                unsafe {
                    rd.buf_ptr = (rd.buf_ptr.offset(n.try_into().unwrap())) as *mut c_char;
                }
                rbytes -= n;
                rdoff = (rdoff + n as u32) % ring.bufsz as u32;

                if rbytes > 0 {
                    continue 'rbytes;
                } else {
                    break 'rbytes;
                }
            }

            ring.rdpending -= 1;
            if ring.rdpending == 0 {
                sigpoll = ring.fillsz == ring.bufsz;
                ring.fillsz -= ring.rdrsvd as usize;
                ring.rdrsvd = 0;
                ring.signal_output(sigpoll);
            }
            break 'outer;
        }
    }

    ring.unlock(flags);

    unsafe {
        rros_schedule();
    }

    ret
}

pub fn do_xbuf_write(ring: &mut XbufRing, wd: &mut XbufWdesc, f_flags: i32) -> i32 {
    let sigpoll: bool;
    let mut flags: u32;
    let mut avail: u32;
    let mut ret: i32;
    let mut wbytes: i32;
    let mut n: i32;
    let mut wroff: u32;
    let mut xret: i32;
    let len: u32 = wd.count.try_into().unwrap();

    if len == 0 {
        return 0;
    }

    if ring.bufsz == 0 {
        return -(bindings::ENOBUFS as i32);
    }

    wd.buf_ptr = wd.buf;
    pr_debug!("do_xbuf_write 1");
    loop {
        flags = ring.lock();
        avail = ring.fillsz as u32 + ring.wrrsvd;
        if avail + len > ring.bufsz as u32 {
            ring.unlock(flags);

            if (f_flags & bindings::O_NONBLOCK as i32) != 0 {
                return -(bindings::EAGAIN as i32);
            }
            ret = ring.wait_output(len.try_into().unwrap());
            if ret != 0 {
                return ret;
            }

            continue;
        }

        wroff = ring.wroff;
        ring.wroff = (wroff + len) % ring.bufsz as u32;
        ring.wrpending += 1;
        ring.wrrsvd += len;
        ret = len.try_into().unwrap();
        wbytes = ret;

        'wbytes: loop {
            if wroff as i32 + wbytes > ring.bufsz.try_into().unwrap() {
                n = ring.bufsz as i32 - wroff as i32;
            } else {
                n = wbytes;
            }

            ring.unlock(flags);

            xret = wd.xfer(
                (ring.bufmem as usize + wroff as usize) as *mut c_char,
                n.try_into().unwrap(),
            );
            pr_debug!("the value of xret is {}", xret);
            flags = ring.lock();
            if xret != 0 {
                ret = -(bindings::EFAULT as i32);
                break 'wbytes;
            }

            unsafe {
                wd.buf_ptr = (wd.buf_ptr.offset(n.try_into().unwrap())) as *mut c_char;
            }
            wbytes -= n;
            wroff = (wroff + n as u32) % ring.bufsz as u32;

            if wbytes > 0 {
                continue 'wbytes;
            } else {
                break 'wbytes;
            }
        }

        ring.wrpending -= 1;
        if ring.wrpending == 0 {
            sigpoll = ring.fillsz == 0;
            ring.fillsz += ring.wrrsvd as usize;
            ring.wrrsvd = 0;
            ring.signal_input(sigpoll);
        }

        ring.unlock(flags);
        break;
    }

    pr_debug!("do_xbuf_write 3, after loop");
    unsafe {
        rros_schedule();
    }
    pr_debug!("the ret of do_xbuf_write is {}", ret);

    ret
}

pub fn inbound_lock(_ring: &XbufRing) -> u32 {
    raw_spin_lock_irqsave().try_into().unwrap()
}

pub fn inbound_unlock(_ring: &XbufRing, flags: u32) {
    raw_spin_unlock_irqrestore(flags.into());
}

extern "C" {
    fn rust_helper_wait_event_interruptible(
        wq_head: *mut bindings::wait_queue_head,
        condition: bool,
    ) -> i32;
}

pub fn wait_event_interruptible(wq_head: *mut bindings::wait_queue_head, condition: bool) -> i32 {
    unsafe { rust_helper_wait_event_interruptible(wq_head, condition) }
}

pub fn inbound_wait_input(ring: &XbufRing, len: usize, avail: usize) -> i32 {
    let xbuf = kernel::container_of!(ring, RrosXbuf, ibnd.ring) as *mut RrosXbuf;
    let ibnd: &mut XbufInbound = unsafe { &mut (*xbuf).ibnd };
    let flags: u32;
    let o_blocked: bool;

    if avail > 0 {
        flags = raw_spin_lock_irqsave().try_into().unwrap();

        o_blocked = ibnd.o_event.wait.wake_up_head().is_some();
        raw_spin_unlock_irqrestore(flags.into());
        if o_blocked {
            return -(bindings::EAGAIN as i32);
        }
    }

    wait_event_interruptible(
        &ibnd.i_event as *const _ as *mut bindings::wait_queue_head,
        ring.fillsz >= len,
    )
}

#[no_mangle]
pub unsafe extern "C" fn c_resume_inband_reader(work: *mut bindings::irq_work) {
    resume_inband_reader(work);
}

#[no_mangle]
pub unsafe extern "C" fn c_resume_inband_writer(work: *mut bindings::irq_work) {
    resume_inband_writer(work);
}

pub fn resume_inband_reader(work: *mut bindings::irq_work) {
    let xbuf = kernel::container_of!(work, RrosXbuf, ibnd.irq_work) as *mut RrosXbuf;

    unsafe {
        bindings::__wake_up(
            &mut (*xbuf).ibnd.i_event as *mut bindings::wait_queue_head,
            bindings::TASK_NORMAL,
            1,
            0 as *mut c_void,
        );
    }
}

// ring locked, irqsoff
pub fn inbound_signal_input(ring: &XbufRing, _sigpoll: bool) {
    let xbuf = kernel::container_of!(ring, RrosXbuf, ibnd.ring) as *mut RrosXbuf;

    let _ret = unsafe { (&mut (*xbuf).ibnd.irq_work).irq_work_queue() };
}

pub fn inbound_wait_output(ring: &XbufRing, _len: usize) -> i32 {
    let xbuf = kernel::container_of!(ring, RrosXbuf, ibnd.ring) as *mut RrosXbuf;

    unsafe { (*xbuf).ibnd.o_event.wait() }
}

pub fn inbound_signal_output(ring: &XbufRing, sigpoll: bool) {
    let xbuf = kernel::container_of!(ring, RrosXbuf, ibnd.ring) as *mut RrosXbuf;
    if sigpoll {
        // unsafe{ rros_signal_poll_events((*xbuf).poll_head, POLLOUT | POLLWRNORM) };
    }

    unsafe { (*xbuf).ibnd.o_event.raise() }
}

pub fn xbuf_read<T: IoBufferWriter>(filp: *mut bindings::file, data: &mut T) -> i32 {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let xbuf = unsafe { (*((*fbind).element)).pointer as *mut RrosXbuf };

    let mut rd: XbufRdesc = XbufRdesc {
        buf: data as *mut _ as *mut c_char,
        buf_ptr: 0 as *mut c_char,
        count: data.len(),
        xfer: Some(write_to_user),
    };

    unsafe {
        do_xbuf_read(
            &mut (*xbuf).ibnd.ring,
            &mut rd,
            (*filp).f_flags.try_into().unwrap(),
        )
    }
}

pub fn xbuf_write<T: IoBufferReader>(filp: *mut bindings::file, data: &mut T) -> i32 {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let xbuf = unsafe { (*((*fbind).element)).pointer as *mut RrosXbuf };

    let mut wd: XbufWdesc = XbufWdesc {
        buf: data as *mut _ as *const c_char,
        buf_ptr: 0 as *mut c_char,
        count: data.len(),
        xfer: Some(read_from_user),
    };

    pr_debug!("before do_xbuf_write");
    unsafe {
        do_xbuf_write(
            &mut (*xbuf).obnd.ring,
            &mut wd,
            (*filp).f_flags.try_into().unwrap(),
        )
    }
}

#[allow(dead_code)]
pub fn xbuf_ioctl(_filp: *mut bindings::file, _cmd: u32, _arg: u32) -> i32 {
    -(bindings::ENOTTY as i32)
}

// static __poll_t xbuf_poll(struct file *filp, poll_table *wait)
// {
// 	struct rros_xbuf *xbuf = element_of(filp, struct rros_xbuf);
// 	struct xbuf_outbound *obnd = &xbuf->obnd;
// 	struct xbuf_inbound *ibnd = &xbuf->ibnd;
// 	unsigned long flags;
// 	__poll_t ready = 0;

// 	poll_wait(filp, &ibnd->i_event, wait);
// 	poll_wait(filp, &obnd->o_event, wait);

// 	flags = ibnd->ring.lock(&ibnd->ring);

// 	if (ibnd->ring.fillsz > 0)
// 		ready |= POLLIN|POLLRDNORM;

// 	ibnd->ring.unlock(&ibnd->ring, flags);

// 	flags = obnd->ring.lock(&obnd->ring);

// 	if (obnd->ring.fillsz < obnd->ring.bufsz)
// 		ready |= POLLOUT|POLLWRNORM;

// 	obnd->ring.unlock(&obnd->ring, flags);

// 	return ready;
// }

#[allow(dead_code)]
pub fn xbuf_oob_ioctl(_filp: *mut bindings::file, _cmd: u32, _arg: u32) -> i32 {
    -(bindings::ENOTTY as i32)
}

pub fn outbound_lock(_ring: &XbufRing) -> u32 {
    raw_spin_lock_irqsave().try_into().unwrap()
}

pub fn outbound_unlock(_ring: &XbufRing, flags: u32) {
    raw_spin_unlock_irqrestore(flags.into());
}

extern "C" {
    fn rust_helper_wq_has_sleeper(wq_head: *mut bindings::wait_queue_head) -> bool;
}

pub fn outbound_wait_input(ring: &XbufRing, len: usize, avail: usize) -> i32 {
    let xbuf = kernel::container_of!(ring, RrosXbuf, obnd.ring) as *mut RrosXbuf;
    let obnd: &mut XbufOutbound = unsafe { &mut (*xbuf).obnd };

    if avail > 0
        && unsafe {
            rust_helper_wq_has_sleeper(&obnd.o_event as *const _ as *mut bindings::wait_queue_head)
        }
    {
        return -(bindings::EAGAIN as i32);
    }

    obnd.i_event
        .wait_timeout(RROS_INFINITE, RrosTmode::RrosRel, || {
            if ring.fillsz >= len {
                true
            } else {
                false
            }
        })
}

pub fn resume_inband_writer(work: *mut bindings::irq_work) {
    let xbuf = kernel::container_of!(work, RrosXbuf, obnd.irq_work) as *mut RrosXbuf;

    unsafe {
        bindings::__wake_up(
            &mut (*xbuf).obnd.o_event as *mut bindings::wait_queue_head,
            bindings::TASK_NORMAL,
            1,
            0 as *mut c_void,
        );
    }
}

// obnd.i_event locked, hard irqs off
pub fn outbound_signal_input(ring: &XbufRing, sigpoll: bool) {
    let xbuf = kernel::container_of!(ring, RrosXbuf, obnd.ring) as *mut RrosXbuf;

    if sigpoll {
        // { rros_signal_poll_events((*xbuf).poll_head, POLLIN | POLLRDNORM); }
    }

    unsafe {
        (*xbuf).obnd.i_event.flush_locked(0);
    }
}

pub fn outbound_wait_output(ring: &XbufRing, len: usize) -> i32 {
    let xbuf = kernel::container_of!(ring, RrosXbuf, ibnd.ring) as *mut RrosXbuf;

    unsafe {
        wait_event_interruptible(
            &(*xbuf).obnd.o_event as *const _ as *mut bindings::wait_queue_head,
            ring.fillsz + len <= ring.bufsz,
        )
    }
}

pub fn outbound_signal_output(ring: &XbufRing, _sigpoll: bool) {
    let xbuf = kernel::container_of!(ring, RrosXbuf, obnd.ring) as *mut RrosXbuf;

    let _ret = unsafe { (&mut (*xbuf).obnd.irq_work).irq_work_queue() };
}

pub fn xbuf_oob_read<T: IoBufferWriter>(filp: *mut bindings::file, data: &mut T) -> i32 {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let xbuf = unsafe { (*((*fbind).element)).pointer as *mut RrosXbuf };

    let mut rd: XbufRdesc = XbufRdesc {
        buf: data as *mut _ as *mut c_char,
        buf_ptr: 0 as *mut c_char,
        count: data.len(),
        xfer: Some(write_to_user),
    };

    unsafe {
        do_xbuf_read(
            &mut (*xbuf).obnd.ring,
            &mut rd,
            (*filp).f_flags.try_into().unwrap(),
        )
    }
}

pub fn xbuf_oob_write<T: IoBufferReader>(filp: *mut bindings::file, data: &mut T) -> i32 {
    let fbind: *const RrosFileBinding = unsafe { (*filp).private_data as *const RrosFileBinding };
    let xbuf = unsafe { (*((*fbind).element)).pointer as *mut RrosXbuf };

    let mut wd: XbufWdesc = XbufWdesc {
        buf: data as *mut _ as *const c_char,
        buf_ptr: 0 as *mut c_char,
        count: data.len(),
        xfer: Some(read_from_user),
    };

    unsafe {
        do_xbuf_write(
            &mut (*xbuf).ibnd.ring,
            &mut wd,
            (*filp).f_flags.try_into().unwrap(),
        )
    }
}

// static __poll_t xbuf_oob_poll(struct file *filp, struct oob_poll_wait *wait)
// {
// 	struct rros_xbuf *xbuf = element_of(filp, struct rros_xbuf);
// 	struct xbuf_outbound *obnd = &xbuf->obnd;
// 	struct xbuf_inbound *ibnd = &xbuf->ibnd;
// 	unsigned long flags;
// 	__poll_t ready = 0;

// 	rros_poll_watch(&xbuf->poll_head, wait, NULL);

// 	flags = obnd->ring.lock(&obnd->ring);

// 	if (obnd->ring.fillsz > 0)
// 		ready |= POLLIN|POLLRDNORM;

// 	obnd->ring.unlock(&obnd->ring, flags);

// 	flags = ibnd->ring.lock(&ibnd->ring);

// 	if (ibnd->ring.fillsz < ibnd->ring.bufsz)
// 		ready |= POLLOUT|POLLWRNORM;

// 	ibnd->ring.unlock(&ibnd->ring, flags);

// 	return ready;
// }

// static int xbuf_release(struct inode *inode, struct file *filp)
// {
// 	struct rros_xbuf *xbuf = element_of(filp, struct rros_xbuf);

// 	rros_flush_wait(&xbuf->obnd.i_event, T_RMID);
// 	rros_flush_flag(&xbuf->ibnd.o_event, T_RMID);

// 	return rros_release_element(inode, filp);
// }

#[allow(dead_code)]
pub fn rros_get_xbuf(rfd: u32, rfilpp: &mut *mut RrosFile) -> Option<NonNull<RrosXbuf>> {
    let rfilp = rros_get_file(rfd);
    match rfilp {
        Some(rfilp) => {
            // unsafe{ (*rfilpp) = Arc::into_raw(rfilp) as *mut RrosFile };
            (*rfilpp) = rfilp.as_ptr();
            let fbind: *const RrosFileBinding =
                unsafe { (*(*(*rfilpp)).filp).private_data as *const RrosFileBinding };
            unsafe {
                Some(NonNull::new_unchecked(
                    (*(*fbind).element).pointer as *mut RrosXbuf,
                ))
            }
        }
        None => None,
    }
}

#[allow(dead_code)]
pub fn rros_put_xbuf(rfilp: &mut RrosFile) {
    let _ret = rros_put_file(rfilp);
}

#[allow(dead_code)]
pub fn rros_read_xbuf(xbuf: &mut RrosXbuf, buf: *mut u8, count: usize, f_flags: i32) -> i32 {
    let mut rd: XbufRdesc = XbufRdesc {
        buf: buf as *mut c_char,
        buf_ptr: 0 as *mut c_char,
        count,
        xfer: Some(write_to_kernel),
    };

    if (f_flags & bindings::O_NONBLOCK as i32) == 0 && rros_cannot_block() {
        return -(bindings::EPERM as i32);
    }

    do_xbuf_read(&mut xbuf.obnd.ring, &mut rd, f_flags)
}

#[allow(dead_code)]
pub fn rros_write_xbuf(xbuf: &mut RrosXbuf, buf: *const i8, count: usize, f_flags: i32) -> i32 {
    let mut wd: XbufWdesc = XbufWdesc {
        buf,
        buf_ptr: 0 as *mut c_char,
        count,
        xfer: Some(read_from_kernel),
    };

    if (f_flags & bindings::O_NONBLOCK as i32) == 0 && rros_cannot_block() {
        return -(bindings::EPERM as i32);
    }

    do_xbuf_write(&mut xbuf.ibnd.ring, &mut wd, f_flags)
}

extern "C" {
    #[allow(improper_ctypes)]
    pub fn __init_waitqueue_head(
        wq_head: *mut bindings::wait_queue_head,
        name: *const c_char,
        arg1: *mut bindings::lock_class_key,
    );
}

fn xbuf_factory_build(
    fac: &'static mut SpinLock<RrosFactory>,
    uname: &'static CStr,
    u_attrs: Option<*mut u8>,
    clone_flags: i32,
    _state_offp: &u32,
) -> Rc<RefCell<RrosElement>> {
    let attrs = RrosXbufAttrs::from_ptr(u_attrs.unwrap() as *mut RrosXbufAttrs);
    if (clone_flags & !RROS_CLONE_PUBLIC) != 0 {
        pr_err!("this is a wrong value");
        // return Err(Error::EINVAL);
    }

    let xbuf = RrosXbuf::new();
    match xbuf {
        Ok(ref _o) => {
            pr_debug!("there is a uninited xbuf");
        }
        Err(_e) => {
            pr_err!("new xbuf error");
            // return Err(Error::ENOMEM);
        }
    }
    let boxed_xbuf = Box::try_new(xbuf.unwrap()).unwrap();
    let xbuf_ptr = Box::into_raw(boxed_xbuf);

    unsafe {
        let ret = rros_init_user_element((*xbuf_ptr).element.clone(), fac, uname, clone_flags);
        if let Err(_e) = ret {
            pr_err!("init user element failed");
        }

        /* Inbound traffic: oob_write() -> read(). */
        let key1 = bindings::lock_class_key::default();
        let name1 =
            CStr::from_bytes_with_nul_unchecked("XBUF RING IBOUND WAITQUEUE HEAD\0".as_bytes());
        __init_waitqueue_head(
            &(*xbuf_ptr).ibnd.i_event as *const _ as *mut bindings::wait_queue_head,
            name1.as_ptr() as *const i8,
            &key1 as *const _ as *mut bindings::lock_class_key,
        );

        (*xbuf_ptr).ibnd.o_event.init();
        raw_spin_lock_init(&mut (*xbuf_ptr).ibnd.lock);
        let _ret = (*xbuf_ptr)
            .ibnd
            .irq_work
            .init_irq_work(c_resume_inband_reader);
        (*xbuf_ptr).ibnd.ring.bufsz = attrs.i_bufsz as usize;
        (*xbuf_ptr).ibnd.ring.bufmem = c_kzalloc(attrs.i_bufsz as u64).unwrap() as *mut u8;
        (*xbuf_ptr).ibnd.ring.lock = Some(inbound_lock);
        (*xbuf_ptr).ibnd.ring.unlock = Some(inbound_unlock);
        (*xbuf_ptr).ibnd.ring.wait_input = Some(inbound_wait_input);
        (*xbuf_ptr).ibnd.ring.signal_input = Some(inbound_signal_input);
        (*xbuf_ptr).ibnd.ring.wait_output = Some(inbound_wait_output);
        (*xbuf_ptr).ibnd.ring.signal_output = Some(inbound_signal_output);

        /* Outbound traffic: write() -> oob_read(). */
        (*xbuf_ptr).obnd.i_event.init(
            &mut RROS_MONO_CLOCK as *mut RrosClock,
            RROS_WAIT_PRIO as i32,
        );
        let key2 = bindings::lock_class_key::default();
        let _name2 =
            CStr::from_bytes_with_nul_unchecked("XBUF RING OUTBOUND WAITQUEUE HEAD\0".as_bytes());
        __init_waitqueue_head(
            &(*xbuf_ptr).obnd.o_event as *const _ as *mut bindings::wait_queue_head,
            uname.as_ptr() as *const i8,
            &key2 as *const _ as *mut bindings::lock_class_key,
        );

        let _ret = (*xbuf_ptr)
            .obnd
            .irq_work
            .init_irq_work(c_resume_inband_writer);
        (*xbuf_ptr).obnd.ring.bufsz = attrs.o_bufsz as usize;
        (*xbuf_ptr).obnd.ring.bufmem = c_kzalloc(attrs.o_bufsz as u64).unwrap() as *mut u8;
        (*xbuf_ptr).obnd.ring.lock = Some(outbound_lock);
        (*xbuf_ptr).obnd.ring.unlock = Some(outbound_unlock);
        (*xbuf_ptr).obnd.ring.wait_input = Some(outbound_wait_input);
        (*xbuf_ptr).obnd.ring.signal_input = Some(outbound_signal_input);
        (*xbuf_ptr).obnd.ring.wait_output = Some(outbound_wait_output);
        (*xbuf_ptr).obnd.ring.signal_output = Some(outbound_signal_output);

        //rros_init_poll_head(&xbuf->poll_head);
        //c_kzfree(xbuf.ibnd.ring.bufmem);
        //c_kzfree(xbuf.obnd.ring.bufmem);

        (*(*xbuf_ptr).element.borrow_mut()).pointer = xbuf_ptr as *mut u8;
        (*xbuf_ptr).element.clone()
    }
}

pub static mut RROS_XBUF_FACTORY: SpinLock<RrosFactory> = unsafe {
    SpinLock::new(RrosFactory {
        name: CStr::from_bytes_with_nul_unchecked("RROS_XBUF_DEV\0".as_bytes()),
        // fops: Some(RustFileXbuf),
        nrdev: CONFIG_RROS_NR_XBUFS,
        build: Some(xbuf_factory_build),
        dispose: Some(xbuf_factory_dispose),
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

pub fn xbuf_factory_dispose(_ele: RrosElement) {}
