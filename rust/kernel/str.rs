// SPDX-License-Identifier: GPL-2.0

//! String representations.

use alloc::alloc::AllocError;
use alloc::vec::Vec;
use core::fmt::{self, Write};
use core::ops::{self, Deref, Index};

use crate::{
    bindings,
    error::{code::*, Error},
};

/// Byte string without UTF-8 validity guarantee.
///
/// `BStr` is simply an alias to `[u8]`, but has a more evident semantical meaning.
pub type BStr = [u8];

/// Creates a new [`BStr`] from a string literal.
///
/// `b_str!` converts the supplied string literal to byte string, so non-ASCII
/// characters can be included.
///
/// # Examples
///
/// ```
/// # use kernel::b_str;
/// # use kernel::str::BStr;
/// const MY_BSTR: &BStr = b_str!("My awesome BStr!");
/// ```
#[macro_export]
macro_rules! b_str {
    ($str:literal) => {{
        const S: &'static str = $str;
        const C: &'static $crate::str::BStr = S.as_bytes();
        C
    }};
}

/// Possible errors when using conversion functions in [`CStr`].
#[derive(Debug, Clone, Copy)]
pub enum CStrConvertError {
    /// Supplied bytes contain an interior `NUL`.
    InteriorNul,

    /// Supplied bytes are not terminated by `NUL`.
    NotNulTerminated,
}

impl From<CStrConvertError> for Error {
    #[inline]
    fn from(_: CStrConvertError) -> Error {
        EINVAL
    }
}

/// A string that is guaranteed to have exactly one `NUL` byte, which is at the
/// end.
///
/// Used for interoperability with kernel APIs that take C strings.
#[repr(transparent)]
pub struct CStr([u8]);

impl CStr {
    /// Returns the length of this string excluding `NUL`.
    #[inline]
    pub const fn len(&self) -> usize {
        self.len_with_nul() - 1
    }

    /// Returns the length of this string with `NUL`.
    #[inline]
    pub const fn len_with_nul(&self) -> usize {
        // SAFETY: This is one of the invariant of `CStr`.
        // We add a `unreachable_unchecked` here to hint the optimizer that
        // the value returned from this function is non-zero.
        if self.0.is_empty() {
            unsafe { core::hint::unreachable_unchecked() };
        }
        self.0.len()
    }

    /// Returns `true` if the string only includes `NUL`.
    #[inline]
    pub const fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Wraps a raw C string pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `NUL`-terminated C string, and it must
    /// last at least `'a`. When `CStr` is alive, the memory pointed by `ptr`
    /// must not be mutated.
    #[inline]
    pub unsafe fn from_char_ptr<'a>(ptr: *const core::ffi::c_char) -> &'a Self {
        // SAFETY: The safety precondition guarantees `ptr` is a valid pointer
        // to a `NUL`-terminated C string.
        let len = unsafe { bindings::strlen(ptr) } + 1;
        // SAFETY: Lifetime guaranteed by the safety precondition.
        let bytes = unsafe { core::slice::from_raw_parts(ptr as _, len as _) };
        // SAFETY: As `len` is returned by `strlen`, `bytes` does not contain interior `NUL`.
        // As we have added 1 to `len`, the last byte is known to be `NUL`.
        unsafe { Self::from_bytes_with_nul_unchecked(bytes) }
    }

    /// Creates a [`CStr`] from a `[u8]`.
    ///
    /// The provided slice must be `NUL`-terminated, does not contain any
    /// interior `NUL` bytes.
    pub const fn from_bytes_with_nul(bytes: &[u8]) -> Result<&Self, CStrConvertError> {
        if bytes.is_empty() {
            return Err(CStrConvertError::NotNulTerminated);
        }
        if bytes[bytes.len() - 1] != 0 {
            return Err(CStrConvertError::NotNulTerminated);
        }
        let mut i = 0;
        // `i + 1 < bytes.len()` allows LLVM to optimize away bounds checking,
        // while it couldn't optimize away bounds checks for `i < bytes.len() - 1`.
        while i + 1 < bytes.len() {
            if bytes[i] == 0 {
                return Err(CStrConvertError::InteriorNul);
            }
            i += 1;
        }
        // SAFETY: We just checked that all properties hold.
        Ok(unsafe { Self::from_bytes_with_nul_unchecked(bytes) })
    }

    /// Creates a [`CStr`] from a `[u8]` without performing any additional
    /// checks.
    ///
    /// # Safety
    ///
    /// `bytes` *must* end with a `NUL` byte, and should only have a single
    /// `NUL` byte (or the string will be truncated).
    #[inline]
    pub const unsafe fn from_bytes_with_nul_unchecked(bytes: &[u8]) -> &CStr {
        // SAFETY: Properties of `bytes` guaranteed by the safety precondition.
        unsafe { core::mem::transmute(bytes) }
    }

    /// Returns a C pointer to the string.
    #[inline]
    pub const fn as_char_ptr(&self) -> *const core::ffi::c_char {
        self.0.as_ptr() as _
    }

    /// Convert the string to a byte slice without the trailing 0 byte.
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        &self.0[..self.len()]
    }

    /// Convert the string to a byte slice containing the trailing 0 byte.
    #[inline]
    pub const fn as_bytes_with_nul(&self) -> &[u8] {
        &self.0
    }

    /// Yields a [`&str`] slice if the [`CStr`] contains valid UTF-8.
    ///
    /// If the contents of the [`CStr`] are valid UTF-8 data, this
    /// function will return the corresponding [`&str`] slice. Otherwise,
    /// it will return an error with details of where UTF-8 validation failed.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::str::CStr;
    /// let cstr = CStr::from_bytes_with_nul(b"foo\0").unwrap();
    /// assert_eq!(cstr.to_str(), Ok("foo"));
    /// ```
    #[inline]
    pub fn to_str(&self) -> Result<&str, core::str::Utf8Error> {
        core::str::from_utf8(self.as_bytes())
    }

    /// Unsafely convert this [`CStr`] into a [`&str`], without checking for
    /// valid UTF-8.
    ///
    /// # Safety
    ///
    /// The contents must be valid UTF-8.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::str::CStr;
    /// // SAFETY: String literals are guaranteed to be valid UTF-8
    /// // by the Rust compiler.
    /// let bar = c_str!("ツ");
    /// assert_eq!(unsafe { bar.as_str_unchecked() }, "ツ");
    /// ```
    #[inline]
    pub unsafe fn as_str_unchecked(&self) -> &str {
        unsafe { core::str::from_utf8_unchecked(self.as_bytes()) }
    }

    /// Convert this [`CStr`] into a [`CString`] by allocating memory and
    /// copying over the string data.
    pub fn to_cstring(&self) -> Result<CString, AllocError> {
        CString::try_from(self)
    }
}

impl fmt::Display for CStr {
    /// Formats printable ASCII characters, escaping the rest.
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::fmt;
    /// # use kernel::str::CStr;
    /// # use kernel::str::CString;
    /// let penguin = c_str!("🐧");
    /// let s = CString::try_from_fmt(fmt!("{}", penguin)).unwrap();
    /// assert_eq!(s.as_bytes_with_nul(), "\\xf0\\x9f\\x90\\xa7\0".as_bytes());
    ///
    /// let ascii = c_str!("so \"cool\"");
    /// let s = CString::try_from_fmt(fmt!("{}", ascii)).unwrap();
    /// assert_eq!(s.as_bytes_with_nul(), "so \"cool\"\0".as_bytes());
    /// ```
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for &c in self.as_bytes() {
            if (0x20..0x7f).contains(&c) {
                // Printable character.
                f.write_char(c as char)?;
            } else {
                write!(f, "\\x{:02x}", c)?;
            }
        }
        Ok(())
    }
}

impl fmt::Debug for CStr {
    /// Formats printable ASCII characters with a double quote on either end, escaping the rest.
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::fmt;
    /// # use kernel::str::CStr;
    /// # use kernel::str::CString;
    /// let penguin = c_str!("🐧");
    /// let s = CString::try_from_fmt(fmt!("{:?}", penguin)).unwrap();
    /// assert_eq!(s.as_bytes_with_nul(), "\"\\xf0\\x9f\\x90\\xa7\"\0".as_bytes());
    ///
    /// // Embedded double quotes are escaped.
    /// let ascii = c_str!("so \"cool\"");
    /// let s = CString::try_from_fmt(fmt!("{:?}", ascii)).unwrap();
    /// assert_eq!(s.as_bytes_with_nul(), "\"so \\\"cool\\\"\"\0".as_bytes());
    /// ```
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("\"")?;
        for &c in self.as_bytes() {
            match c {
                // Printable characters.
                b'\"' => f.write_str("\\\"")?,
                0x20..=0x7e => f.write_char(c as char)?,
                _ => write!(f, "\\x{:02x}", c)?,
            }
        }
        f.write_str("\"")
    }
}

impl AsRef<BStr> for CStr {
    #[inline]
    fn as_ref(&self) -> &BStr {
        self.as_bytes()
    }
}

impl Deref for CStr {
    type Target = BStr;

    #[inline]
    fn deref(&self) -> &Self::Target {
        self.as_bytes()
    }
}

impl Index<ops::RangeFrom<usize>> for CStr {
    type Output = CStr;

    #[inline]
    fn index(&self, index: ops::RangeFrom<usize>) -> &Self::Output {
        // Delegate bounds checking to slice.
        // Assign to _ to mute clippy's unnecessary operation warning.
        let _ = &self.as_bytes()[index.start..];
        // SAFETY: We just checked the bounds.
        unsafe { Self::from_bytes_with_nul_unchecked(&self.0[index.start..]) }
    }
}

impl Index<ops::RangeFull> for CStr {
    type Output = CStr;

    #[inline]
    fn index(&self, _index: ops::RangeFull) -> &Self::Output {
        self
    }
}

mod private {
    use core::ops;

    // Marker trait for index types that can be forward to `BStr`.
    pub trait CStrIndex {}

    impl CStrIndex for usize {}
    impl CStrIndex for ops::Range<usize> {}
    impl CStrIndex for ops::RangeInclusive<usize> {}
    impl CStrIndex for ops::RangeToInclusive<usize> {}
}

impl<Idx> Index<Idx> for CStr
where
    Idx: private::CStrIndex,
    BStr: Index<Idx>,
{
    type Output = <BStr as Index<Idx>>::Output;

    #[inline]
    fn index(&self, index: Idx) -> &Self::Output {
        &self.as_bytes()[index]
    }
}

/// Creates a new [`CStr`] from a string literal.
///
/// The string literal should not contain any `NUL` bytes.
///
/// # Examples
///
/// ```
/// # use kernel::c_str;
/// # use kernel::str::CStr;
/// const MY_CSTR: &CStr = c_str!("My awesome CStr!");
/// ```
#[macro_export]
macro_rules! c_str {
    ($str:expr) => {{
        const S: &str = concat!($str, "\0");
        const C: &$crate::str::CStr = match $crate::str::CStr::from_bytes_with_nul(S.as_bytes()) {
            Ok(v) => v,
            Err(_) => panic!("string contains interior NUL"),
        };
        C
    }};
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cstr_to_str() {
        let good_bytes = b"\xf0\x9f\xa6\x80\0";
        let checked_cstr = CStr::from_bytes_with_nul(good_bytes).unwrap();
        let checked_str = checked_cstr.to_str().unwrap();
        assert_eq!(checked_str, "🦀");
    }

    #[test]
    #[should_panic]
    fn test_cstr_to_str_panic() {
        let bad_bytes = b"\xc3\x28\0";
        let checked_cstr = CStr::from_bytes_with_nul(bad_bytes).unwrap();
        checked_cstr.to_str().unwrap();
    }

    #[test]
    fn test_cstr_as_str_unchecked() {
        let good_bytes = b"\xf0\x9f\x90\xA7\0";
        let checked_cstr = CStr::from_bytes_with_nul(good_bytes).unwrap();
        let unchecked_str = unsafe { checked_cstr.as_str_unchecked() };
        assert_eq!(unchecked_str, "🐧");
    }
}

/// Allows formatting of [`fmt::Arguments`] into a raw buffer.
///
/// It does not fail if callers write past the end of the buffer so that they can calculate the
/// size required to fit everything.
///
/// # Invariants
///
/// The memory region between `pos` (inclusive) and `end` (exclusive) is valid for writes if `pos`
/// is less than `end`.
pub(crate) struct RawFormatter {
    // Use `usize` to use `saturating_*` functions.
    beg: usize,
    pos: usize,
    end: usize,
}

impl RawFormatter {
    /// Creates a new instance of [`RawFormatter`] with an empty buffer.
    fn new() -> Self {
        // INVARIANT: The buffer is empty, so the region that needs to be writable is empty.
        Self {
            beg: 0,
            pos: 0,
            end: 0,
        }
    }

    /// Creates a new instance of [`RawFormatter`] with the given buffer pointers.
    ///
    /// # Safety
    ///
    /// If `pos` is less than `end`, then the region between `pos` (inclusive) and `end`
    /// (exclusive) must be valid for writes for the lifetime of the returned [`RawFormatter`].
    pub(crate) unsafe fn from_ptrs(pos: *mut u8, end: *mut u8) -> Self {
        // INVARIANT: The safety requirements guarantee the type invariants.
        Self {
            beg: pos as _,
            pos: pos as _,
            end: end as _,
        }
    }

    /// Creates a new instance of [`RawFormatter`] with the given buffer.
    ///
    /// # Safety
    ///
    /// The memory region starting at `buf` and extending for `len` bytes must be valid for writes
    /// for the lifetime of the returned [`RawFormatter`].
    pub(crate) unsafe fn from_buffer(buf: *mut u8, len: usize) -> Self {
        let pos = buf as usize;
        // INVARIANT: We ensure that `end` is never less then `buf`, and the safety requirements
        // guarantees that the memory region is valid for writes.
        Self {
            pos,
            beg: pos,
            end: pos.saturating_add(len),
        }
    }

    /// Returns the current insert position.
    ///
    /// N.B. It may point to invalid memory.
    pub(crate) fn pos(&self) -> *mut u8 {
        self.pos as _
    }

    /// Return the number of bytes written to the formatter.
    pub(crate) fn bytes_written(&self) -> usize {
        self.pos - self.beg
    }
}

impl fmt::Write for RawFormatter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        // `pos` value after writing `len` bytes. This does not have to be bounded by `end`, but we
        // don't want it to wrap around to 0.
        let pos_new = self.pos.saturating_add(s.len());

        // Amount that we can copy. `saturating_sub` ensures we get 0 if `pos` goes past `end`.
        let len_to_copy = core::cmp::min(pos_new, self.end).saturating_sub(self.pos);

        if len_to_copy > 0 {
            // SAFETY: If `len_to_copy` is non-zero, then we know `pos` has not gone past `end`
            // yet, so it is valid for write per the type invariants.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    s.as_bytes().as_ptr(),
                    self.pos as *mut u8,
                    len_to_copy,
                )
            };
        }

        self.pos = pos_new;
        Ok(())
    }
}

/// Allows formatting of [`fmt::Arguments`] into a raw buffer.
///
/// Fails if callers attempt to write more than will fit in the buffer.
pub(crate) struct Formatter(RawFormatter);

impl Formatter {
    /// Creates a new instance of [`Formatter`] with the given buffer.
    ///
    /// # Safety
    ///
    /// The memory region starting at `buf` and extending for `len` bytes must be valid for writes
    /// for the lifetime of the returned [`Formatter`].
    pub(crate) unsafe fn from_buffer(buf: *mut u8, len: usize) -> Self {
        // SAFETY: The safety requirements of this function satisfy those of the callee.
        Self(unsafe { RawFormatter::from_buffer(buf, len) })
    }
}

impl Deref for Formatter {
    type Target = RawFormatter;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl fmt::Write for Formatter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.0.write_str(s)?;

        // Fail the request if we go past the end of the buffer.
        if self.0.pos > self.0.end {
            Err(fmt::Error)
        } else {
            Ok(())
        }
    }
}

/// An owned string that is guaranteed to have exactly one `NUL` byte, which is at the end.
///
/// Used for interoperability with kernel APIs that take C strings.
///
/// # Invariants
///
/// The string is always `NUL`-terminated and contains no other `NUL` bytes.
///
/// # Examples
///
/// ```
/// use kernel::{str::CString, fmt};
///
/// let s = CString::try_from_fmt(fmt!("{}{}{}", "abc", 10, 20)).unwrap();
/// assert_eq!(s.as_bytes_with_nul(), "abc1020\0".as_bytes());
///
/// let tmp = "testing";
/// let s = CString::try_from_fmt(fmt!("{tmp}{}", 123)).unwrap();
/// assert_eq!(s.as_bytes_with_nul(), "testing123\0".as_bytes());
///
/// // This fails because it has an embedded `NUL` byte.
/// let s = CString::try_from_fmt(fmt!("a\0b{}", 123));
/// assert_eq!(s.is_ok(), false);
/// ```
pub struct CString {
    buf: Vec<u8>,
}

impl CString {
    /// Creates an instance of [`CString`] from the given formatted arguments.
    pub fn try_from_fmt(args: fmt::Arguments<'_>) -> Result<Self, Error> {
        // Calculate the size needed (formatted string plus `NUL` terminator).
        let mut f = RawFormatter::new();
        f.write_fmt(args)?;
        f.write_str("\0")?;
        let size = f.bytes_written();

        // Allocate a vector with the required number of bytes, and write to it.
        let mut buf = Vec::try_with_capacity(size)?;
        // SAFETY: The buffer stored in `buf` is at least of size `size` and is valid for writes.
        let mut f = unsafe { Formatter::from_buffer(buf.as_mut_ptr(), size) };
        f.write_fmt(args)?;
        f.write_str("\0")?;

        // SAFETY: The number of bytes that can be written to `f` is bounded by `size`, which is
        // `buf`'s capacity. The contents of the buffer have been initialised by writes to `f`.
        unsafe { buf.set_len(f.bytes_written()) };

        // Check that there are no `NUL` bytes before the end.
        // SAFETY: The buffer is valid for read because `f.bytes_written()` is bounded by `size`
        // (which the minimum buffer size) and is non-zero (we wrote at least the `NUL` terminator)
        // so `f.bytes_written() - 1` doesn't underflow.
        let ptr = unsafe { bindings::memchr(buf.as_ptr().cast(), 0, (f.bytes_written() - 1) as _) };
        if !ptr.is_null() {
            return Err(EINVAL);
        }

        // INVARIANT: We wrote the `NUL` terminator and checked above that no other `NUL` bytes
        // exist in the buffer.
        Ok(Self { buf })
    }
}

impl Deref for CString {
    type Target = CStr;

    fn deref(&self) -> &Self::Target {
        // SAFETY: The type invariants guarantee that the string is `NUL`-terminated and that no
        // other `NUL` bytes exist.
        unsafe { CStr::from_bytes_with_nul_unchecked(self.buf.as_slice()) }
    }
}

impl<'a> TryFrom<&'a CStr> for CString {
    type Error = AllocError;

    fn try_from(cstr: &'a CStr) -> Result<CString, AllocError> {
        let mut buf = Vec::new();

        buf.try_extend_from_slice(cstr.as_bytes_with_nul())
            .map_err(|_| AllocError)?;

        // INVARIANT: The `CStr` and `CString` types have the same invariants for
        // the string data, and we copied it over without changes.
        Ok(CString { buf })
    }
}

/// A convenience alias for [`core::format_args`].
#[macro_export]
macro_rules! fmt {
    ($($f:tt)*) => ( core::format_args!($($f)*) )
}
