use crate::bindings;

pub struct KernelOldTimespec {
    pub spec: bindings::__kernel_old_timespec,
}

impl KernelOldTimespec {
    pub fn new() -> Self {
        Self { spec: bindings::__kernel_old_timespec::default() }
    }
}

pub struct KernelTimespec {
    pub spec: bindings::__kernel_timespec,
}

impl KernelTimespec {
    pub fn new() -> Self {
        Self { spec: bindings::__kernel_timespec::default() }
    }
    
    // fn set_tv_sec(tv_sec: bindings::__kernel_old_time_t) {
    //     self.spec.tv_sec = tv_sec;
    // }
}