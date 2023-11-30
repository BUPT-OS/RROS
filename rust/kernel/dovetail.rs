// SPDX-License-Identifier: GPL-2.0

//! Dovetail fucntions.
use crate::{bindings, prelude::*};
/// The `dovetail_start` function is a wrapper around the `bindings::dovetail_start` function from the kernel bindings. It starts the Dovetail interface in the kernel.
///
/// This function does not take any arguments. It calls the `bindings::dovetail_start` function and checks the return value. If the return value is 0, it returns `Ok(0)`. Otherwise, it returns `Err(Error::EINVAL)`.
///
/// This function is unsafe because it calls an unsafe function from the kernel bindings.
pub fn dovetail_start() -> Result<usize> {
    let res = unsafe { bindings::dovetail_start() };
    if res == 0 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}
