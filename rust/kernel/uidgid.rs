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

/// Struct `KgidT` represents a kernel group ID.
/// It wraps the `kgid_t` struct from the bindings module.
/// It includes a public field which is an instance of `kgid_t`.
#[derive(Copy, Clone)]
pub struct KgidT(pub bindings::kgid_t);

impl KuidT {
    /// Takes a pointer to a `u8` and returns a `KuidT` struct.
    pub fn from_ptr(ptr: *const u8) -> Self {
        unsafe { Self((*(ptr as *const bindings::inode)).i_uid) }
    }

    /// Returns a `KuidT` struct with the value of 0.
    /// Corresponds to the C macro GLOBAL_ROOT_UID.
    pub fn global_root_uid() -> Self {
        Self(bindings::kuid_t { val: 0 })
    }
}


impl KgidT {
    /// Takes a pointer to a `u8` and returns a `KgidT` struct.
    pub fn from_ptr(ptr: *const u8) -> Self {
        unsafe { Self((*(ptr as *const bindings::inode)).i_gid) }
    }

    /// Returns a `KgidT` struct with the value of 0.
    /// Corresponds to the C macro GLOBAL_ROOT_GID.
    pub fn global_root_gid() -> Self {
        Self(bindings::kgid_t { val: 0 })
    }
}
