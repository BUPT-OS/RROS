// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/device.h`](../../../../include/linux/device.h)

use crate::{bindings, c_types};
/// The `DeviceType` struct is a wrapper around the `bindings::device_type` struct from the kernel bindings. It represents a device type in the kernel.
pub struct DeviceType(bindings::device_type);

impl DeviceType {
    /// The `new` method is a constructor for `DeviceType`. It creates a new `DeviceType` with a default `bindings::device_type`.
    pub fn new() -> Self {
        Self(bindings::device_type::default())
    }

    /// The `name` method sets the name of the device type. It takes a pointer to a C-style string and sets the `name` field of the underlying `bindings::device_type` to that string.
    pub fn name(mut self, name: *const c_types::c_char) -> Self {
        (self.0).name = name;
        self
    }

    /// The `devnode` method sets the `devnode` function pointer of the device type. It takes an optional function pointer that is called to get the devnode of the device. If the function pointer is `None`, it sets the `devnode` field of the underlying `bindings::device_type` to null.
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

/// The `Device` struct is a wrapper around the `bindings::device` struct from the kernel bindings. It represents a device in the kernel.
pub struct Device(bindings::device);
