// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/irq_pipeline.h`](../../../../include/linux/irq_pipeline.h)

extern "C" {
    fn rust_helper_irq_send_oob_ipi(ipi: usize, cpumask: *const cpumask::CpumaskT);
    fn rust_helper_irq_get_TIMER_OOB_IPI() -> usize;
}

use crate::cpumask;

/// `irq_send_oob_ipi`: A wrapper around `rust_helper_irq_send_oob_ipi` that sends an out-of-band IPI to the CPUs specified by the `cpumask`. It takes an IPI number and a pointer to a `cpumask::CpumaskT`.
pub fn irq_send_oob_ipi(ipi: usize, cpumask: *const cpumask::CpumaskT) {
    unsafe { rust_helper_irq_send_oob_ipi(ipi, cpumask) };
}

/// `irq_get_timer_oob_ipi`: A wrapper around `rust_helper_irq_get_TIMER_OOB_IPI` that returns the IPI number for the timer out-of-band interrupt.
pub fn irq_get_timer_oob_ipi() -> usize {
    unsafe { rust_helper_irq_get_TIMER_OOB_IPI() }
}
