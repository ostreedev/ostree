use crate::{Repo, RepoCheckoutFilterResult};
use glib::ffi::gpointer;
use glib::translate::*;
use libc::c_char;
use std::any::Any;
use std::panic::catch_unwind;
use std::path::{Path, PathBuf};
use std::process::abort;

/// A filter callback to decide which files to checkout from a [Repo](struct.Repo.html). The
/// function is called for every directory and file in the dirtree.
///
/// # Arguments
/// * `repo` - the `Repo` that is being checked out
/// * `path` - the path of the current file, as an absolute path rooted at the commit's root. The
///   root directory is '/', a subdir would be '/subdir' etc.
/// * `stat` - the metadata of the current file
///
/// # Return Value
/// The return value determines whether the current file is checked out or skipped.
pub struct RepoCheckoutFilter(Box<dyn Fn(&Repo, &Path, &libc::stat) -> RepoCheckoutFilterResult>);

impl RepoCheckoutFilter {
    /// Wrap a closure for use as a filter function.
    ///
    /// # Return Value
    /// The return value is always `Some` containing the value. It simply comes pre-wrapped for your
    /// convenience.
    pub fn new<F>(closure: F) -> Option<RepoCheckoutFilter>
    where
        F: (Fn(&Repo, &Path, &libc::stat) -> RepoCheckoutFilterResult) + 'static,
    {
        Some(RepoCheckoutFilter(Box::new(closure)))
    }

    /// Call the contained closure.
    fn call(&self, repo: &Repo, path: &Path, stat: &libc::stat) -> RepoCheckoutFilterResult {
        self.0(repo, path, stat)
    }
}

impl<'a> ToGlibPtr<'a, gpointer> for RepoCheckoutFilter {
    type Storage = ();

    fn to_glib_none(&'a self) -> Stash<gpointer, Self> {
        Stash(self as *const RepoCheckoutFilter as gpointer, ())
    }
}

impl FromGlibPtrNone<gpointer> for &RepoCheckoutFilter {
    // `ptr` must be valid for the lifetime of the returned reference.
    unsafe fn from_glib_none(ptr: gpointer) -> Self {
        assert!(!ptr.is_null());
        &*(ptr as *const RepoCheckoutFilter)
    }
}

/// Trampoline to be called by libostree that calls the Rust closure in the `user_data` parameter.
///
/// # Safety
/// All parameters must be valid pointers for the runtime of the function. In particular,
/// `user_data` must point to a [RepoCheckoutFilter](struct.RepoCheckoutFilter.html) value.
///
/// # Panics
/// If any parameter is a null pointer, the function panics.
unsafe fn filter_trampoline(
    repo: *mut ffi::OstreeRepo,
    path: *const c_char,
    stat: *mut libc::stat,
    user_data: gpointer,
) -> ffi::OstreeRepoCheckoutFilterResult {
    // We can't guarantee it's a valid pointer, but we can make sure it's not null.
    assert!(!stat.is_null());
    let stat = &*stat;
    // This reference is valid until the end of this function, which is shorter than the lifetime
    // of `user_data` so we're fine.
    let closure: &RepoCheckoutFilter = from_glib_none(user_data);
    // `repo` lives at least until the end of this function. This means we can just borrow the
    // reference so long as our `repo` is not moved out of this function.
    let repo = from_glib_borrow(repo);
    // This is a copy so no problems here.
    let path: PathBuf = from_glib_none(path);

    let result = closure.call(&repo, &path, stat);
    result.into_glib()
}

/// Unwind-safe trampoline to call the Rust filter callback. See [filter_trampoline](fn.filter_trampoline.html).
/// This function additionally catches panics and aborts to avoid unwinding into C code.
pub(super) unsafe extern "C" fn filter_trampoline_unwindsafe(
    repo: *mut ffi::OstreeRepo,
    path: *const c_char,
    stat: *mut libc::stat,
    user_data: gpointer,
) -> ffi::OstreeRepoCheckoutFilterResult {
    // Unwinding across an FFI boundary is Undefined Behavior and we have no other way to communicate
    // the error. We abort() safely to avoid further problems.
    let result = catch_unwind(move || filter_trampoline(repo, path, stat, user_data));
    result.unwrap_or_else(|panic| {
        print_panic(panic);
        abort()
    })
}

/// Print a panic message and the value to stderr, if we can.
///
/// If the panic value is either `&str` or `String`, we print it. Otherwise, we don't.
fn print_panic(panic: Box<dyn Any>) {
    eprintln!("A Rust callback invoked by C code panicked.");
    eprintln!("Unwinding across FFI boundaries is Undefined Behavior so abort() will be called.");
    let msg = {
        if let Some(s) = panic.as_ref().downcast_ref::<&str>() {
            s
        } else if let Some(s) = panic.as_ref().downcast_ref::<String>() {
            s
        } else {
            "UNABLE TO SHOW VALUE OF PANIC"
        }
    };
    eprintln!("Panic value: {}", msg);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use std::ptr;

    #[test]
    #[should_panic]
    fn trampoline_should_panic_if_repo_is_nullptr() {
        let path = CString::new("/a/b/c").unwrap();
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let filter = RepoCheckoutFilter(Box::new(|_, _, _| RepoCheckoutFilterResult::Allow));
        unsafe {
            filter_trampoline(
                ptr::null_mut(),
                path.as_ptr(),
                &mut stat,
                filter.to_glib_none().0,
            );
        }
    }

    #[test]
    #[should_panic]
    fn trampoline_should_panic_if_path_is_nullptr() {
        let repo = Repo::new_default();
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let filter = RepoCheckoutFilter(Box::new(|_, _, _| RepoCheckoutFilterResult::Allow));
        unsafe {
            filter_trampoline(
                repo.to_glib_none().0,
                ptr::null(),
                &mut stat,
                filter.to_glib_none().0,
            );
        }
    }

    #[test]
    #[should_panic]
    fn trampoline_should_panic_if_stat_is_nullptr() {
        let repo = Repo::new_default();
        let path = CString::new("/a/b/c").unwrap();
        let filter = RepoCheckoutFilter(Box::new(|_, _, _| RepoCheckoutFilterResult::Allow));
        unsafe {
            filter_trampoline(
                repo.to_glib_none().0,
                path.as_ptr(),
                ptr::null_mut(),
                filter.to_glib_none().0,
            );
        }
    }

    #[test]
    #[should_panic]
    fn trampoline_should_panic_if_user_data_is_nullptr() {
        let repo = Repo::new_default();
        let path = CString::new("/a/b/c").unwrap();
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        unsafe {
            filter_trampoline(
                repo.to_glib_none().0,
                path.as_ptr(),
                &mut stat,
                ptr::null_mut(),
            );
        }
    }

    #[test]
    fn trampoline_should_call_the_closure() {
        let repo = Repo::new_default();
        let path = CString::new("/a/b/c").unwrap();
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let filter = {
            let repo = repo.clone();
            let path = path.clone();
            RepoCheckoutFilter(Box::new(move |arg_repo, arg_path, _| {
                assert_eq!(arg_repo, &repo);
                assert_eq!(&CString::new(arg_path.to_str().unwrap()).unwrap(), &path);
                RepoCheckoutFilterResult::Skip
            }))
        };
        let result = unsafe {
            filter_trampoline(
                repo.to_glib_none().0,
                path.as_ptr(),
                &mut stat,
                filter.to_glib_none().0,
            )
        };
        assert_eq!(result, ffi::OSTREE_REPO_CHECKOUT_FILTER_SKIP);
    }
}
