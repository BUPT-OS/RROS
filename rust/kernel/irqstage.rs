// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/irqstage.h`](../../../../include/linux/irqstage.h)

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
};

pub fn enable_oob_stage(name: *const c_types::c_char) -> Result<usize> {
    let res = unsafe { bindings::enable_oob_stage(name) };
    if res == 0 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}
