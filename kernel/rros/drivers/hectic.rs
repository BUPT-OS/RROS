use core::{cell::UnsafeCell, result::Result::Err};

use alloc::sync::{Arc, Weak};

use crate::{
    file::{rros_open_file, rros_release_file, RrosFile},
    flags::RrosFlag,
    stax::RrosStax,
    thread::RrosKthread,
    timer::*,
};

use kernel::{
    c_str, chrdev, file_operations, irq_work::IrqWork, prelude::*, sync::SpinLock, KernelModule,
};

const RROS_HECIOC_LOCK_STAX: i32 = 18441;
const RROS_HECIOC_UNLOCK_STAX: i32 = 18442;

module! {
    type: Hecticdev,
    name: b"hectic",
    author: b"wxg",
    description: b"hectic driver",
    license: b"GPL v2",
}

#[repr(C)]
struct HecticTaskIndex {
    index: u32,
    flags: u32,
}

#[repr(C)]
struct HecticSwitchReq {
    from: u32,
    to: u32,
}

#[allow(dead_code)]
struct HecticError {
    last_switch: HecticSwitchReq,
    fp_val: u32,
}

#[allow(dead_code)]
struct RtswitchTask {
    base: HecticTaskIndex,
    rt_synch: RrosFlag,
    //TODO: nrt_synch: semaphore
    kthread: RrosKthread,
    last_switch: u32,
    ctx: Option<Weak<RtswitchTask>>,
}

#[allow(dead_code)]
pub struct RtswitchContext {
    tasks: Vec<Arc<RtswitchTask>>,
    tasks_count: u32,
    next_index: u32,
    //lock: Semaphore,
    cpu: u32,
    switches_count: u32,
    pause_us: u64,
    next_task: u32,
    wake_up_delay: Arc<SpinLock<RrosTimer>>,
    failed: bool,
    error: HecticError,
    utask: Option<Weak<RtswitchTask>>,
    wake_utask: IrqWork,
    stax: RrosStax,
    rfile: RrosFile,
}

impl RtswitchTask {
    #[allow(dead_code)]
    pub fn new() -> Self {
        Self {
            base: HecticTaskIndex { index: 0, flags: 0 },
            rt_synch: RrosFlag::new(),
            kthread: RrosKthread::new(None),
            last_switch: 0,
            ctx: None,
        }
    }
}

impl RtswitchContext {
    fn new() -> Result<Self> {
        let ctx = RtswitchContext {
            tasks: Vec::new(),
            tasks_count: 0,
            next_index: 0,
            //lock: Semaphore::new(),
            cpu: 0,
            switches_count: 0,
            pause_us: 0,
            next_task: 0,
            wake_up_delay: Arc::try_new(unsafe { SpinLock::new(RrosTimer::new(0)) })?,
            failed: false,
            error: HecticError {
                last_switch: HecticSwitchReq {
                    from: u32::MAX,
                    to: u32::MAX,
                },
                fp_val: 0,
            },
            utask: None,
            wake_utask: IrqWork::new(),
            stax: RrosStax::new(),
            rfile: RrosFile::new(),
        };
        // ctx.lock.init(1);
        // unsafe{
        // TODO: some initialization which is not needed by stax test
        //     rros_init_timer_on_rq(ctx.wake_up_delay, &mut RROS_MONO_CLOCK, Some(latmus_irq_handler), null_mut(), c_str!("latmus_irq"), RROS_TIMER_IGRAVITY);
        // }
        // ctx.wake_utask.init_irq_work(rtswitch_utask_waker);
        Ok(ctx)
    }

    fn init(&mut self) -> Result<()> {
        self.stax.init()?;
        return Ok(());
    }
}
pub struct HecticFile;

impl file_operations::FileOpener<u8> for HecticFile {
    fn open(_context: &u8, file: &kernel::file::File) -> kernel::Result<Self::Wrapper> {
        let mut ctx: Arc<UnsafeCell<RtswitchContext>> =
            Arc::try_new(UnsafeCell::new(RtswitchContext::new()?))?;
        Arc::get_mut(&mut ctx).unwrap().get_mut().init()?;
        let rfile = &mut Arc::get_mut(&mut ctx).unwrap().get_mut().rfile;
        rros_open_file(rfile, file.get_ptr())?;
        Ok(unsafe { Pin::new_unchecked(ctx) })
    }
}

impl file_operations::FileOperations for HecticFile {
    kernel::declare_file_operations!(ioctl, oob_ioctl, compat_ioctl, compat_oob_ioctl);

    type Wrapper = Pin<Arc<UnsafeCell<RtswitchContext>>>;

    fn release(obj: Self::Wrapper, _file: &kernel::file::File) {
        let obj = unsafe { Pin::into_inner_unchecked(obj) };
        let ctx = unsafe { &mut *obj.as_ref().get() };
        ctx.stax.destory();
        rros_release_file(&mut ctx.rfile).expect("release file failed");
        pr_info!("hectic release!\n");
    }

    fn ioctl(
        this: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as core::ops::Deref>::Target,
        _file: &kernel::file::File,
        cmd: &mut file_operations::IoctlCommand,
    ) -> kernel::Result<i32> {
        pr_info!("Hectic ioctl\n");
        let ctx = unsafe { &mut *this.get() };
        match cmd.cmd as i32 {
            RROS_HECIOC_LOCK_STAX => match ctx.stax.lock() {
                Ok(_) => {}
                Err(e) => {
                    return Err(e);
                }
            },
            RROS_HECIOC_UNLOCK_STAX => {
                ctx.stax.unlock();
            }
            _ => {}
        }
        Ok(0)
    }

    fn oob_ioctl(
        this: &<<Self::Wrapper as kernel::types::PointerWrapper>::Borrowed as core::ops::Deref>::Target,
        _file: &kernel::file::File,
        cmd: &mut file_operations::IoctlCommand,
    ) -> kernel::Result<i32> {
        pr_info!("Hectic oob_ioctl\n");
        let ctx = unsafe { &mut *this.get() };
        match cmd.cmd as i32 {
            RROS_HECIOC_LOCK_STAX => match ctx.stax.lock() {
                Ok(_) => {}
                Err(e) => {
                    return Err(e);
                }
            },
            RROS_HECIOC_UNLOCK_STAX => {
                ctx.stax.unlock();
            }
            _ => {}
        }
        Ok(0)
    }
}

pub struct Hecticdev {
    pub dev: Pin<Box<chrdev::Registration<1>>>,
}

impl KernelModule for Hecticdev {
    fn init() -> Result<Self> {
        let mut _dev = chrdev::Registration::new_pinned(c_str!("hecticdev"), 0, &THIS_MODULE)?;

        _dev.as_mut().register::<HecticFile>()?;

        Ok(Hecticdev { dev: _dev })
    }
}

impl Drop for Hecticdev {
    fn drop(&mut self) {
        pr_info!("Hectic exit\n");
    }
}
