// SPDX-License-Identifier: GPL-2.0

//! A wrapper for data protected by a lock that does not wrap it.

use super::{lock::Backend, lock::Lock};
use crate::build_assert;
use core::{cell::UnsafeCell, mem::size_of, ptr};

/// Allows access to some data to be serialised by a lock that does not wrap it.
///
/// In most cases, data protected by a lock is wrapped by the appropriate lock type, e.g.,
/// [`super::Mutex`] or [`super::SpinLock`]. [`LockedBy`] is meant for cases when this is not
/// possible. For example, if a container has a lock and some data in the contained elements needs
/// to be protected by the same lock.
///
/// [`LockedBy`] wraps the data in lieu of another locking primitive, and only allows access to it
/// when the caller shows evidence that the 'external' lock is locked. It panics if the evidence
/// refers to the wrong instance of the lock.
///
/// # Examples
///
/// The following is an example for illustrative purposes: `InnerDirectory::bytes_used` is an
/// aggregate of all `InnerFile::bytes_used` and must be kept consistent; so we wrap `InnerFile` in
/// a `LockedBy` so that it shares a lock with `InnerDirectory`. This allows us to enforce at
/// compile-time that access to `InnerFile` is only granted when an `InnerDirectory` is also
/// locked; we enforce at run time that the right `InnerDirectory` is locked.
///
/// ```
/// use kernel::sync::{LockedBy, Mutex};
///
/// struct InnerFile {
///     bytes_used: u64,
/// }
///
/// struct File {
///     _ino: u32,
///     inner: LockedBy<InnerFile, InnerDirectory>,
/// }
///
/// struct InnerDirectory {
///     /// The sum of the bytes used by all files.
///     bytes_used: u64,
///     _files: Vec<File>,
/// }
///
/// struct Directory {
///     _ino: u32,
///     inner: Mutex<InnerDirectory>,
/// }
///
/// /// Prints `bytes_used` from both the directory and file.
/// fn print_bytes_used(dir: &Directory, file: &File) {
///     let guard = dir.inner.lock();
///     let inner_file = file.inner.access(&guard);
///     pr_info!("{} {}", guard.bytes_used, inner_file.bytes_used);
/// }
///
/// /// Increments `bytes_used` for both the directory and file.
/// fn inc_bytes_used(dir: &Directory, file: &File) {
///     let mut guard = dir.inner.lock();
///     guard.bytes_used += 10;
///
///     let file_inner = file.inner.access_mut(&mut guard);
///     file_inner.bytes_used += 10;
/// }
///
/// /// Creates a new file.
/// fn new_file(ino: u32, dir: &Directory) -> File {
///     File {
///         _ino: ino,
///         inner: LockedBy::new(&dir.inner, InnerFile { bytes_used: 0 }),
///     }
/// }
/// ```
pub struct LockedBy<T: ?Sized, U: ?Sized> {
    owner: *const U,
    data: UnsafeCell<T>,
}

// SAFETY: `LockedBy` can be transferred across thread boundaries iff the data it protects can.
unsafe impl<T: ?Sized + Send, U: ?Sized> Send for LockedBy<T, U> {}

// SAFETY: `LockedBy` serialises the interior mutability it provides, so it is `Sync` as long as the
// data it protects is `Send`.
unsafe impl<T: ?Sized + Send, U: ?Sized> Sync for LockedBy<T, U> {}

impl<T, U> LockedBy<T, U> {
    /// Constructs a new instance of [`LockedBy`].
    ///
    /// It stores a raw pointer to the owner that is never dereferenced. It is only used to ensure
    /// that the right owner is being used to access the protected data. If the owner is freed, the
    /// data becomes inaccessible; if another instance of the owner is allocated *on the same
    /// memory location*, the data becomes accessible again: none of this affects memory safety
    /// because in any case at most one thread (or CPU) can access the protected data at a time.
    pub fn new<B: Backend>(owner: &Lock<U, B>, data: T) -> Self {
        build_assert!(
            size_of::<Lock<U, B>>() > 0,
            "The lock type cannot be a ZST because it may be impossible to distinguish instances"
        );
        Self {
            owner: owner.data.get(),
            data: UnsafeCell::new(data),
        }
    }
}

impl<T: ?Sized, U> LockedBy<T, U> {
    /// Returns a reference to the protected data when the caller provides evidence (via a
    /// reference) that the owner is locked.
    ///
    /// `U` cannot be a zero-sized type (ZST) because there are ways to get an `&U` that matches
    /// the data protected by the lock without actually holding it.
    ///
    /// # Panics
    ///
    /// Panics if `owner` is different from the data protected by the lock used in
    /// [`new`](LockedBy::new).
    pub fn access<'a>(&'a self, owner: &'a U) -> &'a T {
        build_assert!(
            size_of::<U>() > 0,
            "`U` cannot be a ZST because `owner` wouldn't be unique"
        );
        if !ptr::eq(owner, self.owner) {
            panic!("mismatched owners");
        }

        // SAFETY: `owner` is evidence that the owner is locked.
        unsafe { &*self.data.get() }
    }

    /// Returns a mutable reference to the protected data when the caller provides evidence (via a
    /// mutable owner) that the owner is locked mutably.
    ///
    /// `U` cannot be a zero-sized type (ZST) because there are ways to get an `&mut U` that
    /// matches the data protected by the lock without actually holding it.
    ///
    /// Showing a mutable reference to the owner is sufficient because we know no other references
    /// can exist to it.
    ///
    /// # Panics
    ///
    /// Panics if `owner` is different from the data protected by the lock used in
    /// [`new`](LockedBy::new).
    pub fn access_mut<'a>(&'a self, owner: &'a mut U) -> &'a mut T {
        build_assert!(
            size_of::<U>() > 0,
            "`U` cannot be a ZST because `owner` wouldn't be unique"
        );
        if !ptr::eq(owner, self.owner) {
            panic!("mismatched owners");
        }

        // SAFETY: `owner` is evidence that there is only one reference to the owner.
        unsafe { &mut *self.data.get() }
    }
}
