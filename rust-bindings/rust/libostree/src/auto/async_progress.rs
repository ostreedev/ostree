// This file was generated by gir (https://github.com/gtk-rs/gir @ ffda6f9)
// from gir-files (https://github.com/gtk-rs/gir-files @ ???)
// DO NOT EDIT

use ffi;
use glib;
use glib::object::Downcast;
use glib::object::IsA;
use glib::signal::SignalHandlerId;
use glib::signal::connect;
use glib::translate::*;
use glib_ffi;
use gobject_ffi;
use std::boxed::Box as Box_;
use std::mem;
use std::mem::transmute;
use std::ptr;

glib_wrapper! {
    pub struct AsyncProgress(Object<ffi::OstreeAsyncProgress, ffi::OstreeAsyncProgressClass>);

    match fn {
        get_type => || ffi::ostree_async_progress_get_type(),
    }
}

impl AsyncProgress {
    ///
    /// # Returns
    ///
    /// A new progress object
    pub fn new() -> AsyncProgress {
        unsafe {
            from_glib_full(ffi::ostree_async_progress_new())
        }
    }

    //pub fn new_and_connect<P: Into<Option</*Unimplemented*/Fundamental: Pointer>>, Q: Into<Option</*Unimplemented*/Fundamental: Pointer>>>(changed: P, user_data: Q) -> AsyncProgress {
    //    unsafe { TODO: call ffi::ostree_async_progress_new_and_connect() }
    //}
}

impl Default for AsyncProgress {
    fn default() -> Self {
        Self::new()
    }
}

/// Trait containing all `AsyncProgress` methods.
///
/// # Implementors
///
/// [`AsyncProgress`](struct.AsyncProgress.html)
pub trait AsyncProgressExt {
    /// Process any pending signals, ensuring the main context is cleared
    /// of sources used by this object. Also ensures that no further
    /// events will be queued.
    fn finish(&self);

    //#[cfg(any(feature = "v2017_6", feature = "dox"))]
    //fn get(&self, : /*Unknown conversion*//*Unimplemented*/Fundamental: VarArgs);

    /// Get the human-readable status string from the `AsyncProgress`. This
    /// operation is thread-safe. The retuned value may be `None` if no status is
    /// set.
    ///
    /// This is a convenience function to get the well-known `status` key.
    ///
    /// Feature: `v2017_6`
    ///
    ///
    /// # Returns
    ///
    /// the current status, or `None` if none is set
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn get_status(&self) -> Option<String>;

    fn get_uint(&self, key: &str) -> u32;

    fn get_uint64(&self, key: &str) -> u64;

    /// Look up a key in the `AsyncProgress` and return the `glib::Variant` associated
    /// with it. The lookup is thread-safe.
    ///
    /// Feature: `v2017_6`
    ///
    /// ## `key`
    /// a key to look up
    ///
    /// # Returns
    ///
    /// value for the given `key`, or `None` if
    ///  it was not set
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn get_variant(&self, key: &str) -> Option<glib::Variant>;

    //#[cfg(any(feature = "v2017_6", feature = "dox"))]
    //fn set(&self, : /*Unknown conversion*//*Unimplemented*/Fundamental: VarArgs);

    /// Set the human-readable status string for the `AsyncProgress`. This
    /// operation is thread-safe. `None` may be passed to clear the status.
    ///
    /// This is a convenience function to set the well-known `status` key.
    ///
    /// Feature: `v2017_6`
    ///
    /// ## `status`
    /// new status string, or `None` to clear the status
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn set_status<'a, P: Into<Option<&'a str>>>(&self, status: P);

    fn set_uint(&self, key: &str, value: u32);

    fn set_uint64(&self, key: &str, value: u64);

    /// Assign a new `value` to the given `key`, replacing any existing value. The
    /// operation is thread-safe. `value` may be a floating reference;
    /// `glib::Variant::ref_sink` will be called on it.
    ///
    /// Any watchers of the `AsyncProgress` will be notified of the change if
    /// `value` differs from the existing value for `key`.
    ///
    /// Feature: `v2017_6`
    ///
    /// ## `key`
    /// a key to set
    /// ## `value`
    /// the value to assign to `key`
    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn set_variant(&self, key: &str, value: &glib::Variant);

    /// Emitted when `self_` has been changed.
    fn connect_changed<F: Fn(&Self) + 'static>(&self, f: F) -> SignalHandlerId;
}

impl<O: IsA<AsyncProgress> + IsA<glib::object::Object>> AsyncProgressExt for O {
    fn finish(&self) {
        unsafe {
            ffi::ostree_async_progress_finish(self.to_glib_none().0);
        }
    }

    //#[cfg(any(feature = "v2017_6", feature = "dox"))]
    //fn get(&self, : /*Unknown conversion*//*Unimplemented*/Fundamental: VarArgs) {
    //    unsafe { TODO: call ffi::ostree_async_progress_get() }
    //}

    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn get_status(&self) -> Option<String> {
        unsafe {
            from_glib_full(ffi::ostree_async_progress_get_status(self.to_glib_none().0))
        }
    }

    fn get_uint(&self, key: &str) -> u32 {
        unsafe {
            ffi::ostree_async_progress_get_uint(self.to_glib_none().0, key.to_glib_none().0)
        }
    }

    fn get_uint64(&self, key: &str) -> u64 {
        unsafe {
            ffi::ostree_async_progress_get_uint64(self.to_glib_none().0, key.to_glib_none().0)
        }
    }

    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn get_variant(&self, key: &str) -> Option<glib::Variant> {
        unsafe {
            from_glib_full(ffi::ostree_async_progress_get_variant(self.to_glib_none().0, key.to_glib_none().0))
        }
    }

    //#[cfg(any(feature = "v2017_6", feature = "dox"))]
    //fn set(&self, : /*Unknown conversion*//*Unimplemented*/Fundamental: VarArgs) {
    //    unsafe { TODO: call ffi::ostree_async_progress_set() }
    //}

    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn set_status<'a, P: Into<Option<&'a str>>>(&self, status: P) {
        let status = status.into();
        let status = status.to_glib_none();
        unsafe {
            ffi::ostree_async_progress_set_status(self.to_glib_none().0, status.0);
        }
    }

    fn set_uint(&self, key: &str, value: u32) {
        unsafe {
            ffi::ostree_async_progress_set_uint(self.to_glib_none().0, key.to_glib_none().0, value);
        }
    }

    fn set_uint64(&self, key: &str, value: u64) {
        unsafe {
            ffi::ostree_async_progress_set_uint64(self.to_glib_none().0, key.to_glib_none().0, value);
        }
    }

    #[cfg(any(feature = "v2017_6", feature = "dox"))]
    fn set_variant(&self, key: &str, value: &glib::Variant) {
        unsafe {
            ffi::ostree_async_progress_set_variant(self.to_glib_none().0, key.to_glib_none().0, value.to_glib_none().0);
        }
    }

    fn connect_changed<F: Fn(&Self) + 'static>(&self, f: F) -> SignalHandlerId {
        unsafe {
            let f: Box_<Box_<Fn(&Self) + 'static>> = Box_::new(Box_::new(f));
            connect(self.to_glib_none().0, "changed",
                transmute(changed_trampoline::<Self> as usize), Box_::into_raw(f) as *mut _)
        }
    }
}

unsafe extern "C" fn changed_trampoline<P>(this: *mut ffi::OstreeAsyncProgress, f: glib_ffi::gpointer)
where P: IsA<AsyncProgress> {
    let f: &&(Fn(&P) + 'static) = transmute(f);
    f(&AsyncProgress::from_glib_borrow(this).downcast_unchecked())
}