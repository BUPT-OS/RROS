use crate::{bindings, prelude::*};

pub fn dovetail_start() -> Result<usize> {
    let res = unsafe { bindings::dovetail_start() };
    if res == 0 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}
