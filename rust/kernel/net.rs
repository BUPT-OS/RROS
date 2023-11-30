// SPDX-License-Identifier: GPL-2.0

//! Rros Net module.
use crate::{bindings, str::CStr, ARef, AlwaysRefCounted};
use core::{cell::UnsafeCell, ptr::NonNull};

extern "C" {
    #[allow(improper_ctypes)]
    fn rust_helper_dev_hold(dev: *mut bindings::net_device) -> ();
    #[allow(improper_ctypes)]
    fn rust_helper_dev_put(dev: *mut bindings::net_device) -> ();
    #[allow(improper_ctypes)]
    fn rust_helper_get_net(net: *mut bindings::net) -> *mut bindings::net;
    #[allow(improper_ctypes)]
    fn rust_helper_put_net(net: *mut bindings::net) -> ();
}

/// Wraps the kernel's `struct net_device`.
#[repr(transparent)]
pub struct Device(UnsafeCell<bindings::net_device>);

// SAFETY: Instances of `Device` are created on the C side. They are always refcounted.
unsafe impl AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { rust_helper_dev_hold(self.0.get()) };
    }

    unsafe fn dec_ref(obj: core::ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { rust_helper_dev_put(obj.cast().as_ptr()) };
    }
}

/// Wraps the kernel's `struct net`.
#[repr(transparent)]
pub struct Namespace(UnsafeCell<bindings::net>);

impl Namespace {
    /// Finds a network device with the given name in the namespace.
    pub fn dev_get_by_name(&self, name: &CStr) -> Option<ARef<Device>> {
        // SAFETY: The existence of a shared reference guarantees the refcount is nonzero.
        let ptr =
            NonNull::new(unsafe { bindings::dev_get_by_name(self.0.get(), name.as_char_ptr()) })?;
        Some(unsafe { ARef::from_raw(ptr.cast()) })
    }
}

// SAFETY: Instances of `Namespace` are created on the C side. They are always refcounted.
unsafe impl AlwaysRefCounted for Namespace {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { rust_helper_get_net(self.0.get()) };
    }

    unsafe fn dec_ref(obj: core::ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { rust_helper_put_net(obj.cast().as_ptr()) };
    }
}

/// Returns the network namespace for the `init` process.
pub fn init_ns() -> &'static Namespace {
    unsafe { &*core::ptr::addr_of!(bindings::init_net).cast() }
}
