use kernel::bindings;
use kernel::prelude::*;

#[macro_export]
macro_rules! oob_retval {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[0])
    };
}

#[macro_export]
macro_rules! oob_arg1 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[0])
    };
}

#[macro_export]
macro_rules! oob_arg2 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[1])
    };
}

#[macro_export]
macro_rules! oob_arg3 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[2])
    };
}

#[macro_export]
macro_rules! oob_arg4 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[3])
    };
}

#[macro_export]
macro_rules! oob_arg5 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[4])
    };
}

#[macro_export]
macro_rules! is_clock_gettime {
    ($nr:expr) => {
        (($nr) == bindings::__NR_clock_gettime as i32)
    };
}

#[macro_export]
#[cfg(not(__NR_clock_gettime64))]
macro_rules! is_clock_gettime64 {
    ($nr:expr) => {
        false
    };
}

#[macro_export]
#[cfg(__NR_clock_gettime64)]
macro_rules! is_clock_gettime64 {
    ($nr:expr) => {
        ((nr) == bindings::__NR_clock_gettime64)
    };
}

pub fn is_oob_syscall(regs: *const bindings::pt_regs) -> bool {
    (unsafe { (*regs).syscallno } & bindings::__OOB_SYSCALL_BIT as i32) != 0
}

pub fn oob_syscall_nr(regs: *const bindings::pt_regs) -> u32 {
    pr_info!("the sys call number is {}", (*regs).syscallno as u32);
    (unsafe { (*regs).syscallno as u32 } & !bindings::__OOB_SYSCALL_BIT as u32)
}

pub fn inband_syscall_nr(regs: *mut bindings::pt_regs, nr: *mut u32) -> bool {
    unsafe {
        *nr = oob_syscall_nr(regs);
    }
    !is_oob_syscall(regs)
}

pub fn set_oob_error(regs: *mut bindings::pt_regs, err: i32) {
    unsafe {
        oob_retval!(regs) = err as u64;
    }
}

pub fn set_oob_retval(regs: *mut bindings::pt_regs, err: i64) {
    unsafe {
        oob_retval!(regs) = err as u64;
    }
}
