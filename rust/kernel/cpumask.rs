// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/cpumask.h`](../../../../include/linux/cpumask.h)

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
};
extern "C"{
    fn rust_helper_num_possible_cpus() -> u32;
}
use core::iter::Iterator;

/// An possible CPU index iterator.
///
/// This iterator has a similar abilitiy to the kernel's macro `for_each_possible_cpu`.
pub struct PossibleCpusIndexIter {
    index: i32,
}

/// An online CPU index iterator.
///
/// This iterator has a similar abilitiy to the kernel's macro `for_each_online_cpu`.
pub struct OnlineCpusIndexIter {
    index: i32,
}

/// An present CPU index iterator.
///
/// This iterator has a similar abilitiy to the kernel's macro `for_each_present_cpu`.
pub struct PresentCpusIndexIter {
    index: i32,
}

impl Iterator for PossibleCpusIndexIter {
    type Item = u32;

    fn next(&mut self) -> Option<u32> {
        let next_cpu_id =
            unsafe { bindings::cpumask_next(self.index, &bindings::__cpu_possible_mask) };
        // When [`bindings::cpumask_next`] can not find further CPUs set in the
        // [`bindings::__cpu_possible_mask`], it returns a value >= [`bindings::nr_cpu_ids`].
        if next_cpu_id >= unsafe { bindings::nr_cpu_ids } {
            return None;
        }
        self.index = next_cpu_id as i32;
        Some(next_cpu_id)
    }
}

impl Iterator for OnlineCpusIndexIter {
    type Item = u32;

    fn next(&mut self) -> Option<u32> {
        let next_cpu_id =
            unsafe { bindings::cpumask_next(self.index, &bindings::__cpu_online_mask) };
        // When [`bindings::cpumask_next`] can not find further CPUs set in the
        // [`bindings::__cpu_online_mask`], it returns a value >= [`bindings::nr_cpu_ids`].
        if next_cpu_id >= unsafe { bindings::nr_cpu_ids } {
            return None;
        }
        self.index = next_cpu_id as i32;
        Some(next_cpu_id)
    }
}

impl Iterator for PresentCpusIndexIter {
    type Item = u32;

    fn next(&mut self) -> Option<u32> {
        let next_cpu_id =
            unsafe { bindings::cpumask_next(self.index, &bindings::__cpu_present_mask) };
        // When [`bindings::cpumask_next`] can not find further CPUs set in the
        // [`bindings::__cpu_present_mask`], it returns a value >= [`bindings::nr_cpu_ids`].
        if next_cpu_id >= unsafe { bindings::nr_cpu_ids } {
            return None;
        }
        self.index = next_cpu_id as i32;
        Some(next_cpu_id)
    }
}

/// Returns a [`PossibleCpusIndexIter`] that gives the possible CPU indexes.
///
/// # Examples
///
/// ```
/// # use kernel::prelude::*;
/// # use kernel::cpumask::possible_cpus;
///
/// fn example() {
///     // This prints all the possible cpu indexes.
///     for cpu in possible_cpus(){
///         pr_info!("{}\n", cpu);
///     }
/// }
/// ```
pub fn possible_cpus() -> PossibleCpusIndexIter {
    // Initial index is set to -1. Since [`bindings::cpumask_next`] return the next set bit in a
    // [`bindings::__cpu_possible_mask`], the CPU index should begins from 0.
    PossibleCpusIndexIter { index: -1 }
}

/// Returns a [`OnlineCpusIndexIter`] that gives the online CPU indexes.
///
/// # Examples
///
/// ```
/// # use kernel::prelude::*;
/// # use kernel::cpumask::online_cpus;
///
/// fn example() {
///     // This prints all the online cpu indexes.
///     for cpu in online_cpus(){
///         pr_info!("{}\n", cpu);
///     }
/// }
/// ```
pub fn online_cpus() -> OnlineCpusIndexIter {
    // Initial index is set to -1. Since [`bindings::cpumask_next`] return the next set bit in a
    // [`bindings::__cpu_online_mask`], the CPU index should begins from 0.
    OnlineCpusIndexIter { index: -1 }
}

/// Returns a [`PresentCpusIndexIter`] that gives the present CPU indexes.
///
/// # Examples
///
/// ```
/// # use kernel::prelude::*;
/// # use kernel::cpumask::present_cpus;
///
/// fn example() {
///     // This prints all the present cpu indexes.
///     for cpu in present_cpus(){
///         pr_info!("{}\n", cpu);
///     }
/// }
/// ```
pub fn present_cpus() -> PresentCpusIndexIter {
    // Initial index is set to -1. Since [`bindings::cpumask_next`] return the next set bit in a
    // [`bindings::__cpu_present_mask`], the CPU index should begins from 0.
    PresentCpusIndexIter { index: -1 }
}

extern "C" {
    // #[allow(improper_ctypes)]
    fn rust_helper_cpulist_parse(
        buf: *const c_types::c_char,
        dstp: *mut bindings::cpumask,
    ) -> c_types::c_int;
    fn rust_helper_cpumask_copy(dstp: *mut bindings::cpumask, srcp: *const bindings::cpumask);
    fn rust_helper_cpumask_and(dstp: *mut bindings::cpumask, srcp1: *const bindings::cpumask,
                                srcp2: *const bindings::cpumask);
    fn rust_helper_cpumask_empty(srcp: *const bindings::cpumask) -> c_types::c_int;
    fn rust_helper_cpumask_first(srcp: *const bindings::cpumask);
    //fn rust_helper_per_cpu();
}

// static mut CPU_ONLINE_MASK: bindings::cpumask = unsafe{ bindings::__cpu_online_mask };
pub struct CpumaskT(bindings::cpumask_t);

pub struct CpumaskVarT(bindings::cpumask_var_t);

impl CpumaskT {
    pub const fn from_int(c: u64) -> Self {
        Self(bindings::cpumask_t { bits: [c, 0 , 0, 0] })
        // Self(bindings::cpumask_t { bits: [c] })
    }

    pub fn as_cpumas_ptr(&mut self) -> *mut bindings::cpumask_t {
        &mut self.0 as *mut bindings::cpumask_t
    }

    pub fn cpu_mask_all() -> Self{
        let c: u64 = u64::MAX;
        Self(bindings::cpumask_t { bits: [c, c, c, c] })
        // Self(bindings::cpumask_t { bits: [c] })
    }
}

#[cfg(not(CONFIG_CPUMASK_OFFSTACK))]
impl CpumaskVarT {
    pub const fn from_int(c: u64) -> Self {
        Self([bindings::cpumask_t { bits: [c, 0 , 0, 0] }])
        // Self([bindings::cpumask_t { bits: [c] }])
    }

    pub fn alloc_cpumask_var(mask: &mut CpumaskVarT) -> Result<usize>{
        Ok(0)
    }

    pub fn free_cpumask_var(mask: &mut CpumaskVarT) -> Result<usize>{
        Ok(0)
    }
}


#[cfg(CONFIG_CPUMASK_OFFSTACK)]
impl CpumaskVarT {
    // todo: implement for x86_64/x86
}

pub unsafe fn read_cpu_online_mask() -> bindings::cpumask {
    unsafe { bindings::__cpu_online_mask }
}

pub fn cpulist_parse(buf: *const c_types::c_char, dstp: *mut bindings::cpumask) -> Result<usize> {
    let res = unsafe { rust_helper_cpulist_parse(buf, dstp) };
    if res == 0 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}

pub fn cpumask_copy(dstp: *mut bindings::cpumask, srcp: *const bindings::cpumask) {
    unsafe { rust_helper_cpumask_copy(dstp, srcp) }
}

pub fn cpumask_and(dstp: *mut bindings::cpumask, srcp1: *const bindings::cpumask,
                                srcp2: *const bindings::cpumask) {
    unsafe { rust_helper_cpumask_and(dstp, srcp1, srcp2) }
}


pub fn cpumask_empty(srcp: *const bindings::cpumask, ) -> Result<usize> {
    let res = unsafe { rust_helper_cpumask_empty(srcp) };
    if res == 1 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}

pub fn cpumask_first(srcp: *const bindings::cpumask, ) {
    unsafe { rust_helper_cpumask_first(srcp) }
}

pub fn num_possible_cpus() -> u32 {
    unsafe{rust_helper_num_possible_cpus()}
}
