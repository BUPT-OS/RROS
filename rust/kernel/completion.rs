// SPDX-License-Identifier: GPL-2.0

//! cpumask
//!
//! C header: [`include/linux/completion.h`](../../../../include/linux/completion.h)

use crate::bindings;

extern "C" {
    fn rust_helper_init_completion(x: *mut bindings::completion);
}

pub struct Completion(bindings::completion);

impl Completion {
    pub fn new() -> Self {
        let completion = bindings::completion::default();
        Self(completion)
    }

    pub fn init_completion(&mut self) {
        unsafe { rust_helper_init_completion(&mut self.0 as *mut bindings::completion) }
    }

    pub fn complete(&mut self) {
        unsafe { bindings::complete(&mut self.0 as *mut bindings::completion) }
    }

    pub fn wait_for_completion(&mut self) {
        unsafe { bindings::wait_for_completion(&mut self.0 as *mut bindings::completion) }
    }
}
