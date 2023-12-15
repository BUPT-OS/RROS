// SPDX-License-Identifier: GPL-2.0

//! irqstage
//!
//! C header: [`include/linux/irqstage.h`](../../../../include/linux/irqstage.h)

use crate::bindings;

#[derive(Copy, Clone)]
pub struct PtRegs {
    pub ptr: *mut bindings::pt_regs,
}

impl PtRegs {
    pub fn from_ptr(ptr: *mut bindings::pt_regs) -> Self {
        PtRegs { ptr }
    }
}

#[derive(Copy, Clone)]
pub struct IrqStage {
    pub ptr: *mut bindings::irq_stage,
}

impl IrqStage {
    pub fn get_oob_state() -> Self {
        unsafe {
            IrqStage {
                ptr: &mut bindings::oob_stage as *mut bindings::irq_stage,
            }
        }
    }

    pub fn from_ptr(ptr: *mut bindings::irq_stage) -> Self {
        IrqStage { ptr }
    }
}
