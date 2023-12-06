// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/fs.h`](../../../../include/linux/fs.h)
//!

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
    str::CStr,
};

pub struct Filename(*mut bindings::filename);

impl Filename {
    pub fn getname_kernel(arg1: &'static CStr) -> Result<Self> {
        let res;
        unsafe {
            res = bindings::getname_kernel(arg1.as_char_ptr());
        }
        if res == core::ptr::null_mut() {
            return Err(Error::EINVAL);
        }
        Ok(Self(res))
    }

    pub fn get_name(& self) -> *const c_types::c_char {
        unsafe { (*self.0).name }
    }

    pub fn from_ptr(ptr: *mut bindings::filename) -> Self {
        Self(ptr)
    }
}

impl Drop for Filename {
    fn drop(&mut self) {
        unsafe { bindings::putname(self.0) };
    }
}

pub fn hashlen_string(salt: *const c_types::c_char, filename: *mut Filename) -> u64 {
    unsafe {
        let name = (*filename).get_name();
        bindings::hashlen_string(salt as *const c_types::c_void, name)
    }
}
