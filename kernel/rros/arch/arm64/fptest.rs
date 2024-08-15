use crate::arch::arm64::uapi::fptest::{RROS_ARM64_FPSIMD, RROS_ARM64_SVE};

extern "C" {
    fn rust_helper_system_supports_fpsimd() -> bool;
    fn rust_helper_system_supports_sve() -> bool;
}

#[inline]
#[allow(dead_code)]
pub(crate) fn rros_begin_fpu() -> bool {
    false
}

#[inline]
#[allow(dead_code)]
pub(crate) fn rros_end_fpu() {}

#[inline]
pub(crate) fn rros_detect_fpu() -> u32 {
    let features: u32 = 0;

    if system_supports_fpsimd() {
        return features | RROS_ARM64_FPSIMD;
    }

    if system_supports_sve() {
        return features | RROS_ARM64_SVE;
    }

    features
}

fn system_supports_fpsimd() -> bool {
    unsafe { rust_helper_system_supports_fpsimd() }
}

fn system_supports_sve() -> bool {
    unsafe { rust_helper_system_supports_sve() }
}
