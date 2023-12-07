// SPDX-License-Identifier: GPL-2.0

//! uidgid
//!
//! C header: [`include/linux/uidgid.h`](../../../../include/linux/uidgid.h)

use crate::bindings::{self};

/// Struct `KuidT` represents a kernel user ID.
/// It wraps the `kuid_t` struct from the bindings module.
/// It includes a public field which is an instance of `kuid_t`.
#[derive(Copy, Clone)]
pub struct KuidT(pub bindings::kuid_t);
#[derive(Copy, Clone)]
pub struct KgidT(pub bindings::kgid_t);

impl KuidT {
    pub fn from_ptr(ptr: *const u8) -> Self {
        unsafe { Self((*(ptr as *const bindings::inode)).i_uid) }
    }

    pub fn global_root_uid() -> Self {
        Self(bindings::kuid_t { val: 0 })
    }
}

/// Struct `KgidT` represents a kernel group ID.
/// It wraps the `kgid_t` struct from the bindings module.
/// It includes a public field which is an instance of `kgid_t`.
impl KgidT {
    pub fn from_ptr(ptr: *const u8) -> Self {
        unsafe { Self((*(ptr as *const bindings::inode)).i_gid) }
    }

    pub fn global_root_gid() -> Self {
        Self(bindings::kgid_t { val: 0 })
    }
}
