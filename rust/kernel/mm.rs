// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/mm.h`](../../../../include/linux/mm.h)

use crate::{
    bindings, c_types,
    error::{Error, Result},
    prelude::*,
};

extern "C" {
    // #[allow(improper_ctypes)]
    fn rust_helper_page_align(size: c_types::c_size_t) -> c_types::c_ulong;
    fn rust_helper_page_aligned(size: c_types::c_size_t) -> c_types::c_int;
}

pub fn page_align(size: usize) -> Result<usize> {
    let res = unsafe { rust_helper_page_align(size) };
    if res != 0 {
        return Ok(res as usize);
    }
    Err(Error::EINVAL)
}

pub fn page_aligned(size: usize) -> Result<usize> {
    let res = unsafe { rust_helper_page_aligned(size) };
    if res == 1 {
        return Ok(0);
    }
    Err(Error::EINVAL)
}

pub fn remap_pfn_range(
    vma: *mut bindings::vm_area_struct,
    vaddr: c_types::c_ulong,
    pfn: c_types::c_ulong,
    size: c_types::c_ulong,
    prot: bindings::pgprot_t,
) -> c_types::c_int {
    unsafe { bindings::remap_pfn_range(vma, vaddr, pfn, size, prot) }
}

type PgprotT = bindings::pgprot_t;
pub const PAGE_SHARED: PgprotT = PgprotT { pgprot: 4035 as u64 };