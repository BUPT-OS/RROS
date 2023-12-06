// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/device/class.h`](../../../../include/linux/device/class.h)

use core::u32;

use crate::{bindings, c_types, error::Error, Result};

extern "C" {
    // #[allow(improper_ctypes)]
    fn rust_helper_class_create(
        this_module: &'static crate::ThisModule,
        buf: *const c_types::c_char,
    ) -> *mut bindings::class;
    fn rust_helper_dev_name(dev: *const bindings::device) -> *const c_types::c_char;
}

pub struct DevT(bindings::dev_t);

impl DevT {
    pub fn new(number: u32) -> Self {
        let a: bindings::dev_t = number;
        Self(number)
    }
}

pub struct Class {
    pub ptr: *mut bindings::class,
}

impl Class {
    pub fn new(
        this_module: &'static crate::ThisModule,
        name: *const c_types::c_char,
    ) -> Result<Self> {
        let ptr = class_create(this_module, name);
        if ptr.is_null() {
            return Err(Error::EBADF);
        }
        Ok(Self { ptr })
    }

    // fn add_function_devnode(){

    // }
}

fn class_create(
    this_module: &'static crate::ThisModule,
    name: *const c_types::c_char,
) -> *mut bindings::class {
    unsafe { rust_helper_class_create(this_module, name) }
}

pub trait ClassOps {}
