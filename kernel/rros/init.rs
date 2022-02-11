#![no_std]
#![feature(allocator_api, global_asm)]
use kernel::prelude::*;

module! {
    type: rros,
    name: b"rros",
    author: b"Hongyu Li",
    description: b"A rust realtime os",
    license: b"GPL v2",
}
struct HelloWorld;
impl KernelModule for HelloWorld {
    fn init() -> Result<Self> {
        pr_info!("Hello world from rros!\n");
        Ok(HelloWorld)
    }
}
impl Drop for HelloWorld {
    fn drop(&mut self) {
        pr_info!("Bye world from rros!\n");
    }
}
