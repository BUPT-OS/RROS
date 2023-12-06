use core::{
    mem::size_of, convert::TryInto, result::Result::Ok,
};

use crate::{
    factory::{
        CloneData,
        RrosElement,
        RrosFactory,
        RrosFactoryInside,
    },
    Rros,
};

use kernel::{
    bindings,
    c_types,
    file::File,
    file_operations::{FileOpener, FileOperations, IoctlCommand, IoctlHandler},
    memory_rros::{rros_shm_size, rros_shared_heap},
    mm::{ PAGE_SHARED, remap_pfn_range },
    io_buffer::{ WritableToBytes, IoBufferWriter,},
    prelude::*, 
    str::CStr,
    sync::{Lock, SpinLock, Mutex},
    uidgid::{KgidT, KuidT},
    user_ptr::{UserSlicePtr, UserSlicePtrWriter,},
};

pub const CONFIG_RROS_NR_CONTROL: usize = 1;

pub static mut RROS_CONTROL_FACTORY: SpinLock<RrosFactory> = unsafe {
    SpinLock::new(RrosFactory {
        name: unsafe { CStr::from_bytes_with_nul_unchecked("RROS_CONTROL_DEV\0".as_bytes()) },
        nrdev: CONFIG_RROS_NR_CONTROL, 
        build: None,
        dispose: None,
        attrs: None,
        flags: 0,
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

pub struct ControlOps;

impl FileOpener<u8> for ControlOps {
    fn open(shared : &u8, fileref: &File) -> Result<Self::Wrapper> {
        // there should be some checks
        let mut data = CloneData::default();
        unsafe{ data.ptr = shared as *const u8 as *mut u8; }
        pr_info!("open control device success");
        Ok(Box::try_new(data)?)
    }
}

impl FileOperations for ControlOps {
    kernel::declare_file_operations!(oob_ioctl, ioctl, mmap);

    type Wrapper = Box<CloneData>;

    // fn ioctl
    fn ioctl(
            this: &CloneData,
            file: &File,
            cmd: &mut IoctlCommand,
        ) -> Result<i32> {
        pr_info!("I'm the ioctl ops of the control factory");
        // cmd.dispatch::<Self>(this, file)
        let ret = control_ioctl(file, cmd);
        pr_info!("the value of ret is {}", ret.unwrap());
        ret
    }

    // fn oob_ioctl
    fn oob_ioctl(
            this: &CloneData,
            file: &File,
            cmd: &mut IoctlCommand,
        ) -> Result<i32> {
        pr_info!("I'm the ioctl ops of the control factory");
        let ret = control_common_ioctl(file, cmd);
        ret
    }

    fn mmap(
            this: &CloneData,
            file: &File,
            vma: &mut bindings::vm_area_struct,
        ) -> Result {
        pr_info!("I'm the mmap ops of the control factory");
        let ret = control_mmap(file, vma);
        ret
    }
}

#[repr(C)]
pub struct RrosCoreInfo {
    abi_base: u32,
    abi_current: u32,
    fpu_features: u32,
    shm_size: u64,
}

impl RrosCoreInfo {
    pub fn new() -> Self {
        RrosCoreInfo { 
            abi_base: 0,
            abi_current: 0,
            fpu_features: 0,
            shm_size: 0
        } 
    }
}

unsafe impl WritableToBytes for RrosCoreInfo {}

#[repr(C)]
pub struct RrosCpuState {
    cpu: u32,
    state_ptr: u64,
}

// pub const RROS_CONTROL_IOCBASE: u32 = 'C';

pub const RROS_ABI_BASE: u32 = 23;
pub const RROS_ABI_LEVEL: u32 = 26;

pub const RROS_CTLIOC_GET_COREINFO: u32 = 2149073664;
pub const RROS_CTLIOC_SCHEDCTL: u32 = 3222815489;
pub const RROS_CTLIOC_GET_CPUSTATE: u32 = 2148549378;

extern "C" {
    fn rust_helper_pa(x: usize) -> usize;
}

fn control_ioctl(file: &File, cmd: &mut IoctlCommand) -> Result<i32> {
    let mut info = RrosCoreInfo::new();
    match cmd.cmd {
        RROS_CTLIOC_GET_COREINFO => {
            info.abi_base = RROS_ABI_BASE;
            info.abi_current = RROS_ABI_LEVEL;
            // in arch/arm64/include/asm/rros/fptest.h
            // TODO There should be a function rros_detect_fpu() related to the arm64 architecture, the result of the function is 0.
            info.fpu_features = 0; 
            pr_info!("the value of info.shm_size and RROS_SHM_SIZE is {}, {}", info.shm_size, rros_shm_size);
            unsafe { info.shm_size = rros_shm_size as u64; }
            pr_info!("the value of info.shm_size and RROS_SHM_SIZE is {}, {}", info.shm_size, rros_shm_size);
            // ret = cmd.user_slice.take().ok_or(Error::EINVAL).writer();
            let data = cmd.user_slice.take().ok_or(Error::EINVAL);
            data.unwrap().writer().write(&info)?;
            Ok(0)
        },
        _ => control_common_ioctl(file, cmd)
    }
}

fn control_common_ioctl(file: &File, cmd: &mut IoctlCommand) -> Result<i32> {
    Ok(0)
}

fn control_mmap(file: &File, vma: &mut bindings::vm_area_struct) -> Result {
    let p = unsafe { rros_shared_heap.membase };  
    let pfn: usize =  unsafe { rust_helper_pa(p as usize) } >> bindings::PAGE_SHIFT;
    let len: usize = (vma.vm_end - vma.vm_start) as usize;

    if len != unsafe { rros_shm_size } {
        return Err(Error::EINVAL);
    }

    unsafe { remap_pfn_range(vma as *mut bindings::vm_area_struct, vma.vm_start, pfn.try_into().unwrap(), len.try_into().unwrap(), PAGE_SHARED) };
    Ok(())
}