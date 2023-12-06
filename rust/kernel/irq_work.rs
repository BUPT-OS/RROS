// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/irq_work.h`](../../../../include/linux/irq_work.h)

use crate::{
    bindings,
    error::{Error, Result},
};

extern "C" {
    fn rust_helper_init_irq_work(
        work: *mut bindings::irq_work,
        func: unsafe extern "C" fn(work: *mut bindings::irq_work),
    );
}

pub struct IrqWork(pub bindings::irq_work);

impl IrqWork {
    pub fn new() -> Self {
        let irq_work = bindings::irq_work::default();
        Self(irq_work)
    }

    // pub fn from_ptr<'a>(work: *mut bindings::irq_work) -> &'a IrqWork{
    //     unsafe { &IrqWork((*work).cast()) }
    // }

    pub fn init_irq_work(
        &mut self,
        func: unsafe extern "C" fn(work: *mut bindings::irq_work),
    ) -> Result<usize> {
        unsafe {
            rust_helper_init_irq_work(&mut self.0 as *mut bindings::irq_work, func);
        }
        Ok(0)
    }

    pub fn irq_work_queue(&mut self) -> Result<usize> {
        let res = unsafe { bindings::irq_work_queue(&mut self.0 as *mut bindings::irq_work) };
        if res == true {
            Ok(0)
        } else {
            Err(Error::EINVAL)
        }
    }

    pub fn get_ptr(&mut self) -> *mut bindings::irq_work {
        unsafe { &mut self.0 as *mut bindings::irq_work }
    }
}
