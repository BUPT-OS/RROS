// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/uidgid.h`](../../../../include/linux/uidgid.h)

use crate::bindings;

pub struct KuidT(pub bindings::kuid_t);
pub struct KgidT(pub bindings::kgid_t);
