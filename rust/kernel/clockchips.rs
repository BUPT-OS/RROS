// SPDX-License-Identifier: GPL-2.0

//! clockchips
//!
//! C header: [`include/linux/clockchips.h`](../../../../include/linux/clockchips.h)
use crate::{bindings, c_types, prelude::*};

type ktime_t = i64;
pub struct Clock_Proxy_Device {
    pub ptr: *mut bindings::clock_proxy_device,
}

impl Clock_Proxy_Device {
    pub fn new(cpd: *mut bindings::clock_proxy_device) -> Result<Self> {
        let ptr = cpd;
        if ptr.is_null() {
            pr_warn!("init clock_proxy_device error!");
            return Err(Error::EINVAL);
        }
        Ok(Self { ptr })
    }

    pub fn get_ptr(&self) -> *mut bindings::clock_proxy_device {
        return unsafe { &mut *self.ptr as *mut bindings::clock_proxy_device };
    }

    pub fn get_proxy_device(&self) -> *mut bindings::clock_event_device {
        unsafe { (&mut (*self.ptr).proxy_device) as *mut bindings::clock_event_device }
    }

    pub fn get_real_device(&self) -> *mut bindings::clock_event_device {
        unsafe { ((*self.ptr).real_device) as *mut bindings::clock_event_device }
    }

    pub fn set_handle_oob_event(
        &self,
        func: unsafe extern "C" fn(dev: *mut bindings::clock_event_device),
    ) {
        unsafe { (*self.ptr).handle_oob_event = Some(func) };
    }
}

pub struct Clock_Event_Device {
    pub ptr: *mut bindings::clock_event_device,
}
impl Clock_Event_Device {
    pub fn from_proxy_device(ced: *mut bindings::clock_event_device) -> Result<Self> {
        let ptr = ced;
        if ptr.is_null() {
            pr_warn!("get proxy_device error!");
            return Err(Error::EINVAL);
        }
        Ok(Self { ptr })
    }

    pub fn get_ptr(&self) -> *mut bindings::clock_event_device {
        return unsafe { &mut *self.ptr as *mut bindings::clock_event_device };
    }

    pub fn get_features(&self) -> c_types::c_uint {
        unsafe { (*self.ptr).features }
    }

    pub fn set_features(&self, num: c_types::c_uint) {
        unsafe { (*self.ptr).features = num };
    }

    pub fn set_set_next_ktime(
        &self,
        func: unsafe extern "C" fn(
            expires: ktime_t,
            arg1: *mut bindings::clock_event_device,
        ) -> c_types::c_int,
    ) {
        unsafe { (*self.ptr).set_next_ktime = Some(func) };
    }

    pub fn set_next_ktime(
        &self,
        evt: ktime_t,
        arg1: *mut bindings::clock_event_device,
    ) -> c_types::c_int {
        unsafe {
            if (*self.ptr).set_next_ktime.is_some() {
                return (*self.ptr).set_next_ktime.unwrap()(evt, arg1);
            }
            return 1;
        }
    }

    pub fn get_set_state_oneshot_stopped(
        &self,
    ) -> Option<unsafe extern "C" fn(arg1: *mut bindings::clock_event_device) -> c_types::c_int>
    {
        unsafe { (*self.ptr).set_state_oneshot_stopped }
    }

    pub fn set_set_state_oneshot_stopped(
        &self,
        func: unsafe extern "C" fn(arg1: *mut bindings::clock_event_device) -> c_types::c_int,
    ) {
        unsafe { (*self.ptr).set_state_oneshot_stopped = Some(func) };
    }

    pub fn get_max_delta_ns(&self) -> u64 {
        unsafe { (*self.ptr).max_delta_ns as u64 }
    }

    pub fn get_min_delta_ns(&self) -> u64 {
        unsafe { (*self.ptr).min_delta_ns as u64 }
    }

    pub fn get_mult(&self) -> u32 {
        unsafe { (*self.ptr).mult as u32 }
    }

    pub fn get_shift(&self) -> u32 {
        unsafe { (*self.ptr).shift as u32 }
    }

    pub fn get_min_delta_ticks(&self) -> u64 {
        unsafe { (*self.ptr).min_delta_ticks as u64 }
    }

    pub fn set_next_event(
        &self,
        evt: c_types::c_ulong,
        arg1: *mut bindings::clock_event_device,
    ) -> c_types::c_int {
        unsafe {
            if (*self.ptr).set_next_event.is_some() {
                return (*self.ptr).set_next_event.unwrap()(evt, arg1);
            }
            return 1;
        }
    }
}
