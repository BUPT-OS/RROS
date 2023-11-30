use kernel::bindings;
use kernel::prelude::*;

/// Returns the first register value from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_retval {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[0])
    };
}

/// Returns the first argument (register value) from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_arg1 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[0])
    };
}

/// Returns the second argument (register value) from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_arg2 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[1])
    };
}

/// Returns the third argument (register value) from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_arg3 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[2])
    };
}

/// Returns the fourth argument (register value) from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_arg4 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[3])
    };
}

/// Returns the fifth argument (register value) from the given out-of-bounds pointer.
#[macro_export]
macro_rules! oob_arg5 {
    ($ptr:expr) => {
        ((*$ptr).__bindgen_anon_1.__bindgen_anon_1.regs[4])
    };
}

/// Macro to check if a syscall is `clock_gettime`.
/// This macro takes a syscall number and returns true if it matches the syscall number for `clock_gettime`.
#[macro_export]
macro_rules! is_clock_gettime {
    ($nr:expr) => {
        (($nr) == bindings::__NR_clock_gettime as i32)
    };
}

/// Macro to check if a syscall is `clock_gettime64`.
/// This macro takes a syscall number and returns false if `__NR_clock_gettime64` is not defined.
/// If `__NR_clock_gettime64` is defined, it returns true if the syscall number matches `__NR_clock_gettime64`.
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
    unsafe { pr_debug!("the sys call number is {}", (*regs).syscallno as u32) };
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
