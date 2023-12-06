// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/irq_pipeline.h`](../../../../include/linux/irq_pipeline.h)

extern "C" {
    fn rust_helper_irq_send_oob_ipi(ipi: usize, cpumask: *const cpumask::CpumaskT);
    fn rust_helper_irq_get_TIMER_OOB_IPI() -> usize;
}

use crate::cpumask;

pub fn irq_send_oob_ipi(ipi: usize, cpumask: *const cpumask::CpumaskT) {
    unsafe { rust_helper_irq_send_oob_ipi(ipi, cpumask) };
}

pub fn irq_get_TIMER_OOB_IPI() -> usize {
    unsafe { rust_helper_irq_get_TIMER_OOB_IPI() }
}
