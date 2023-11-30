// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/fs.h`](../../../../include/linux/fs.h)
//!

use crate::{
    bindings, c_types,
    error::{Error, Result},
    str::CStr,
};

/// The `Filename` struct wraps a pointer to a `bindings::filename` from the kernel bindings.
pub struct Filename(*mut bindings::filename);

impl Filename {
    /// `getname_kernel`: A method that takes a reference to a `CStr` and returns a `Result` containing a new `Filename`. It calls the `bindings::getname_kernel` function with the `CStr` as argument. If the function returns a null pointer, it returns `Err(Error::EINVAL)`. Otherwise, it returns `Ok(Filename(res))`.
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

    /// `get_name`: A method that returns a pointer to a `c_char`. It dereferences the `Filename`'s pointer and returns the `name` field.
    pub fn get_name(&self) -> *const c_types::c_char {
        unsafe { (*self.0).name }
    }

    /// `from_ptr`: A method that takes a pointer to a `bindings::filename` and returns a new `Filename` containing the pointer.
    pub fn from_ptr(ptr: *mut bindings::filename) -> Self {
        Self(ptr)
    }
}

impl Drop for Filename {
    fn drop(&mut self) {
        unsafe { bindings::putname(self.0) };
    }
}

/// The `hashlen_string` function is a wrapper around the `bindings::hashlen_string` function from the kernel bindings. It takes a pointer to a `c_char` and a pointer to a `Filename` and returns a `u64`. It gets the name of the `Filename` and calls the `bindings::hashlen_string` function with the `c_char` and the name as arguments.
pub fn hashlen_string(salt: *const c_types::c_char, filename: *mut Filename) -> u64 {
    unsafe {
        let name = (*filename).get_name();
        bindings::hashlen_string(salt as *const c_types::c_void, name)
    }
}
