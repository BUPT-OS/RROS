// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/device.h`](../../../../include/linux/device.h)

use crate::{bindings, c_types};

pub struct DeviceType(bindings::device_type);

impl DeviceType {
    pub fn new() -> Self {
        Self(bindings::device_type::default())
    }

    pub fn name(mut self, name: *const c_types::c_char) -> Self {
        (self.0).name = name;
        self
    }

    pub fn devnode(
        &mut self,
        devnode: Option<
            unsafe extern "C" fn(
                dev: *mut bindings::device,
                mode: *mut bindings::umode_t,
                uid: *mut bindings::kuid_t,
                gid: *mut bindings::kgid_t,
            ) -> *mut c_types::c_char,
        >,
    ) {
        (self.0).devnode = devnode;
    }
}

pub struct Device(bindings::device);
