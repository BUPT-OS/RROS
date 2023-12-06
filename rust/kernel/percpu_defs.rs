use core::i32;

use crate::c_types;

extern "C" {
    fn rust_helper_per_cpu_ptr(
        var: *mut c_types::c_void,
        cpu: c_types::c_int,
    ) -> *mut c_types::c_void;

    fn rust_helper_raw_cpu_ptr(var: *mut c_types::c_void) -> *mut c_types::c_void;

    fn rust_helper_smp_processor_id() -> c_types::c_int;
}

//per_cpu原型：
//#define per_cpu(var, cpu)	(*per_cpu_ptr(&(var), cpu))
//per_cpu返回具体值无法实现，因此只能用per_cpu_ptr来返回指针
pub fn per_cpu_ptr(var: *mut u8, cpu: i32) -> *mut u8 {
    unsafe {
        return rust_helper_per_cpu_ptr(var as *mut c_types::c_void, cpu as c_types::c_int)
            as *mut u8;
    }
}

// We can use generic to implement part of the ability of function per_cpu. But due to the absence of the
// macro define_percpu, this function has little chance to be used.
pub fn per_cpu<T>(var: *mut T, cpu: i32) -> *mut T {
    unsafe {
        return per_cpu_ptr(var as *mut u8, cpu) as *mut T;
    }
}

pub fn raw_cpu_ptr(var: *mut u8) -> *mut u8 {
    unsafe {
        return rust_helper_raw_cpu_ptr(var as *mut c_types::c_void) as *mut u8;
    }
}

pub fn smp_processor_id() -> c_types::c_int {
    unsafe { rust_helper_smp_processor_id() }
}
