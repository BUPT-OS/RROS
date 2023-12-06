// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/premmpt.h`](../../../../include/linux/premmpt.h)

use crate::{
    c_types,
    error::{Error, Result},
};

extern "C" {
    // #[allow(improper_ctypes)]
    fn rust_helper_running_inband() -> c_types::c_int;
}

pub fn running_inband() -> Result<usize> {
    let res = unsafe { rust_helper_running_inband() };
    if res == 1 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}
