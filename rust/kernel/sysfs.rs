// SPDX-License-Identifier: GPL-2.0

//! System control.
//!
//! C header: [`include/linux/sysfs.h`](../../../../include/linux/sysfs.h)

use crate::bindings;
use core;

pub struct Attribute(bindings::attribute);

impl Attribute {
    fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

pub struct AttributeGroup(bindings::attribute_group);

impl AttributeGroup {
    pub fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

// struct rros_clock_gravity {
// 	ktime_t irq;
// 	ktime_t kernel;
// 	ktime_t user;
// };
