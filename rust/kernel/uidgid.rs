// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/uidgid.h`](../../../../include/linux/uidgid.h)

use crate::bindings;

/// Struct `KuidT` represents a kernel user ID.
/// It wraps the `kuid_t` struct from the bindings module.
/// It includes a public field which is an instance of `kuid_t`.
pub struct KuidT(pub bindings::kuid_t);

/// Struct `KgidT` represents a kernel group ID.
/// It wraps the `kgid_t` struct from the bindings module.
/// It includes a public field which is an instance of `kgid_t`.
pub struct KgidT(pub bindings::kgid_t);
