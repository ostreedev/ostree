use crate::{RepoCheckoutMode, RepoCheckoutOverwriteMode, RepoDevInoCache, SePolicy};
use glib::translate::*;
use libc::c_char;
use std::path::PathBuf;

#[cfg(any(feature = "v2018_2", feature = "dox"))]
mod repo_checkout_filter;
#[cfg(any(feature = "v2018_2", feature = "dox"))]
pub use self::repo_checkout_filter::RepoCheckoutFilter;

/// Options for checking out an OSTree commit.
pub struct RepoCheckoutAtOptions {
    /// Checkout mode.
    pub mode: RepoCheckoutMode,
    /// Overwrite mode.
    pub overwrite_mode: RepoCheckoutOverwriteMode,
    /// Deprecated, do not use.
    pub enable_uncompressed_cache: bool,
    /// Perform `fsync()` on checked out files and directories.
    pub enable_fsync: bool,
    /// Handle OCI/Docker style whiteout files.
    pub process_whiteouts: bool,
    /// Require hardlinking.
    pub no_copy_fallback: bool,
    /// Never hardlink; reflink if possible, otherwise full physical copy.
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    pub force_copy: bool,
    /// Suppress mode bits outside of 0775 for directories.
    #[cfg(any(feature = "v2017_7", feature = "dox"))]
    pub bareuseronly_dirs: bool,
    /// Copy zero-sized files rather than hardlinking.
    #[cfg(any(feature = "v2018_9", feature = "dox"))]
    pub force_copy_zerosized: bool,
    /// Only check out this subpath.
    pub subpath: Option<PathBuf>,
    /// A cache from device, inode pairs to checksums.
    pub devino_to_csum_cache: Option<RepoDevInoCache>,
    /// A callback function to decide which files and directories will be checked out from the
    /// repo. See the documentation on [RepoCheckoutFilter](struct.RepoCheckoutFilter.html) for more
    /// information on the signature.
    ///
    /// # Panics
    /// This callback may not panic. If it does, `abort()` will be called to avoid unwinding across
    /// an FFI boundary and into the libostree C code (which is Undefined Behavior). If you prefer to
    /// swallow the panic rather than aborting, you can use `std::panic::catch_unwind` inside your
    /// callback to catch and silence any panics that occur.
    #[cfg(any(feature = "v2018_2", feature = "dox"))]
    pub filter: Option<RepoCheckoutFilter>,
    /// SELinux policy.
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    pub sepolicy: Option<SePolicy>,
    /// When computing security contexts, prefix the path with this value.
    pub sepolicy_prefix: Option<String>,
}

impl Default for RepoCheckoutAtOptions {
    fn default() -> Self {
        RepoCheckoutAtOptions {
            mode: RepoCheckoutMode::None,
            overwrite_mode: RepoCheckoutOverwriteMode::None,
            enable_uncompressed_cache: false,
            enable_fsync: false,
            process_whiteouts: false,
            no_copy_fallback: false,
            #[cfg(feature = "v2017_6")]
            force_copy: false,
            #[cfg(feature = "v2017_7")]
            bareuseronly_dirs: false,
            #[cfg(feature = "v2018_9")]
            force_copy_zerosized: false,
            subpath: None,
            devino_to_csum_cache: None,
            #[cfg(feature = "v2018_2")]
            filter: None,
            #[cfg(feature = "v2017_6")]
            sepolicy: None,
            sepolicy_prefix: None,
        }
    }
}

type StringStash<'a, T> = Stash<'a, *const c_char, Option<T>>;
type WrapperStash<'a, GlibT, WrappedT> = Stash<'a, *mut GlibT, Option<WrappedT>>;

impl<'a> ToGlibPtr<'a, *const ffi::OstreeRepoCheckoutAtOptions> for RepoCheckoutAtOptions {
    #[allow(clippy::type_complexity)]
    type Storage = (
        Box<ffi::OstreeRepoCheckoutAtOptions>,
        StringStash<'a, PathBuf>,
        StringStash<'a, String>,
        WrapperStash<'a, ffi::OstreeRepoDevInoCache, RepoDevInoCache>,
        WrapperStash<'a, ffi::OstreeSePolicy, SePolicy>,
    );

    // We need to make sure that all memory pointed to by the returned pointer is kept alive by
    // either the `self` reference or the returned Stash.
    fn to_glib_none(&'a self) -> Stash<*const ffi::OstreeRepoCheckoutAtOptions, Self> {
        // Creating this struct from zeroed memory is fine since it's `repr(C)` and only contains
        // primitive types. In fact, the libostree docs say to zero the struct. This means we handle
        // the unused bytes correctly.
        // The struct needs to be boxed so the pointer we return remains valid even as the Stash is
        // moved around.
        let mut options =
            Box::new(unsafe { std::mem::zeroed::<ffi::OstreeRepoCheckoutAtOptions>() });
        options.mode = self.mode.into_glib();
        options.overwrite_mode = self.overwrite_mode.into_glib();
        options.enable_uncompressed_cache = self.enable_uncompressed_cache.into_glib();
        options.enable_fsync = self.enable_fsync.into_glib();
        options.process_whiteouts = self.process_whiteouts.into_glib();
        options.no_copy_fallback = self.no_copy_fallback.into_glib();

        #[cfg(feature = "v2017_6")]
        {
            options.force_copy = self.force_copy.into_glib();
        }

        #[cfg(feature = "v2017_7")]
        {
            options.bareuseronly_dirs = self.bareuseronly_dirs.into_glib();
        }

        #[cfg(feature = "v2018_9")]
        {
            options.force_copy_zerosized = self.force_copy_zerosized.into_glib();
        }

        // We keep these complex values alive by returning them in our Stash. Technically, some of
        // these are being kept alive by `self` already, but it's better to be consistent here.
        let subpath = self.subpath.to_glib_none();
        options.subpath = subpath.0;
        let sepolicy_prefix = self.sepolicy_prefix.to_glib_none();
        options.sepolicy_prefix = sepolicy_prefix.0;
        let devino_to_csum_cache = self.devino_to_csum_cache.to_glib_none();
        options.devino_to_csum_cache = devino_to_csum_cache.0;

        #[cfg(feature = "v2017_6")]
        let sepolicy = {
            let sepolicy = self.sepolicy.to_glib_none();
            options.sepolicy = sepolicy.0;
            sepolicy
        };
        #[cfg(not(feature = "v2017_6"))]
        let sepolicy = None.to_glib_none();

        #[cfg(feature = "v2018_2")]
        {
            if let Some(filter) = &self.filter {
                options.filter_user_data = filter.to_glib_none().0;
                options.filter = Some(repo_checkout_filter::filter_trampoline_unwindsafe);
            }
        }

        Stash(
            options.as_ref(),
            (
                options,
                subpath,
                sepolicy_prefix,
                devino_to_csum_cache,
                sepolicy,
            ),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib_sys::{GFALSE, GTRUE};
    use std::ffi::{CStr, CString};
    use std::ptr;

    #[test]
    fn should_convert_default_options() {
        let options = RepoCheckoutAtOptions::default();
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!((*ptr).mode, ffi::OSTREE_REPO_CHECKOUT_MODE_NONE);
            assert_eq!(
                (*ptr).overwrite_mode,
                ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_NONE
            );
            assert_eq!((*ptr).enable_uncompressed_cache, GFALSE);
            assert_eq!((*ptr).enable_fsync, GFALSE);
            assert_eq!((*ptr).process_whiteouts, GFALSE);
            assert_eq!((*ptr).no_copy_fallback, GFALSE);
            #[cfg(feature = "v2017_6")]
            assert_eq!((*ptr).force_copy, GFALSE);
            #[cfg(feature = "v2017_7")]
            assert_eq!((*ptr).bareuseronly_dirs, GFALSE);
            #[cfg(feature = "v2018_9")]
            assert_eq!((*ptr).force_copy_zerosized, GFALSE);
            assert_eq!((*ptr).unused_bools, [GFALSE; 4]);
            assert_eq!((*ptr).subpath, ptr::null());
            assert_eq!((*ptr).devino_to_csum_cache, ptr::null_mut());
            assert_eq!((*ptr).unused_ints, [0; 6]);
            assert_eq!((*ptr).unused_ptrs, [ptr::null_mut(); 3]);
            #[cfg(feature = "v2018_2")]
            assert_eq!((*ptr).filter, None);
            #[cfg(feature = "v2018_2")]
            assert_eq!((*ptr).filter_user_data, ptr::null_mut());
            #[cfg(feature = "v2017_6")]
            assert_eq!((*ptr).sepolicy, ptr::null_mut());
            assert_eq!((*ptr).sepolicy_prefix, ptr::null());
        }
    }

    #[test]
    fn should_convert_non_default_options() {
        let options = RepoCheckoutAtOptions {
            mode: RepoCheckoutMode::User,
            overwrite_mode: RepoCheckoutOverwriteMode::UnionIdentical,
            enable_uncompressed_cache: true,
            enable_fsync: true,
            process_whiteouts: true,
            no_copy_fallback: true,
            #[cfg(feature = "v2017_6")]
            force_copy: true,
            #[cfg(feature = "v2017_7")]
            bareuseronly_dirs: true,
            #[cfg(feature = "v2018_9")]
            force_copy_zerosized: true,
            subpath: Some("sub/path".into()),
            devino_to_csum_cache: Some(RepoDevInoCache::new()),
            #[cfg(feature = "v2018_2")]
            filter: RepoCheckoutFilter::new(|_repo, _path, _stat| {
                crate::RepoCheckoutFilterResult::Skip
            }),
            #[cfg(feature = "v2017_6")]
            sepolicy: Some(
                SePolicy::new(&gio::File::for_path("a/b"), gio::NONE_CANCELLABLE).unwrap(),
            ),
            sepolicy_prefix: Some("prefix".into()),
        };
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!((*ptr).mode, ffi::OSTREE_REPO_CHECKOUT_MODE_USER);
            assert_eq!(
                (*ptr).overwrite_mode,
                ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL
            );
            assert_eq!((*ptr).enable_uncompressed_cache, GTRUE);
            assert_eq!((*ptr).enable_fsync, GTRUE);
            assert_eq!((*ptr).process_whiteouts, GTRUE);
            assert_eq!((*ptr).no_copy_fallback, GTRUE);
            #[cfg(feature = "v2017_6")]
            assert_eq!((*ptr).force_copy, GTRUE);
            #[cfg(feature = "v2017_7")]
            assert_eq!((*ptr).bareuseronly_dirs, GTRUE);
            #[cfg(feature = "v2018_9")]
            assert_eq!((*ptr).force_copy_zerosized, GTRUE);
            assert_eq!((*ptr).unused_bools, [GFALSE; 4]);
            assert_eq!(
                CStr::from_ptr((*ptr).subpath),
                CString::new("sub/path").unwrap().as_c_str()
            );
            assert_eq!(
                (*ptr).devino_to_csum_cache,
                options.devino_to_csum_cache.to_glib_none().0
            );
            assert_eq!((*ptr).unused_ints, [0; 6]);
            assert_eq!((*ptr).unused_ptrs, [ptr::null_mut(); 3]);
            #[cfg(feature = "v2018_2")]
            assert!((*ptr).filter == Some(repo_checkout_filter::filter_trampoline_unwindsafe));
            #[cfg(feature = "v2018_2")]
            assert_eq!(
                (*ptr).filter_user_data,
                options.filter.as_ref().unwrap().to_glib_none().0,
            );
            #[cfg(feature = "v2017_6")]
            assert_eq!((*ptr).sepolicy, options.sepolicy.to_glib_none().0);
            assert_eq!(
                CStr::from_ptr((*ptr).sepolicy_prefix),
                CString::new("prefix").unwrap().as_c_str()
            );
        }
    }
}
