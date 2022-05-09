use ffi::OstreeKernelArgs;
#[cfg(any(feature = "v2019_3", feature = "dox"))]
use glib::object::IsA;
use glib::translate::*;
#[cfg(any(feature = "v2019_3", feature = "dox"))]
use glib::GString;
use std::fmt;
use std::ptr;

glib::wrapper! {
    /// Kernel arguments.
    #[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct KernelArgs(Boxed<ffi::OstreeKernelArgs>);

    match fn {
        copy => |_ptr| unimplemented!(),
        free => |ptr| ffi::ostree_kernel_args_free(ptr),
    }
}

impl KernelArgs {
    /// Add a kernel argument.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append(&mut self, arg: &str) {
        unsafe {
            ffi::ostree_kernel_args_append(self.to_glib_none_mut().0, arg.to_glib_none().0);
        }
    }

    /// Add multiple kernel arguments.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_argv(&mut self, argv: &[&str]) {
        unsafe {
            ffi::ostree_kernel_args_append_argv(self.to_glib_none_mut().0, argv.to_glib_none().0);
        }
    }

    /// Appends each argument that does not have one of `prefixes`.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_argv_filtered(&mut self, argv: &[&str], prefixes: &[&str]) {
        unsafe {
            ffi::ostree_kernel_args_append_argv_filtered(
                self.to_glib_none_mut().0,
                argv.to_glib_none().0,
                prefixes.to_glib_none().0,
            );
        }
    }

    /// Append the entire contents of the currently booted kernel commandline.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_proc_cmdline<P: IsA<gio::Cancellable>>(
        &mut self,
        cancellable: Option<&P>,
    ) -> Result<(), glib::Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ffi::ostree_kernel_args_append_proc_cmdline(
                self.to_glib_none_mut().0,
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(())
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    /// Remove a kernel argument.
    pub fn delete(&mut self, arg: &str) -> Result<(), glib::Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ffi::ostree_kernel_args_delete(
                self.to_glib_none_mut().0,
                arg.to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(())
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    /// Remove a kernel argument.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn delete_key_entry(&mut self, key: &str) -> Result<(), glib::Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ffi::ostree_kernel_args_delete_key_entry(
                self.to_glib_none_mut().0,
                key.to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(())
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    /// Given `foo`, return the last the value of a `foo=bar` key as `bar`.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn get_last_value(&self, key: &str) -> Option<GString> {
        unsafe {
            from_glib_none(ffi::ostree_kernel_args_get_last_value(
                self.to_glib_none().0 as *mut OstreeKernelArgs,
                key.to_glib_none().0,
            ))
        }
    }

    /// Replace any existing `foo=bar` with `foo=other` e.g.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn new_replace(&mut self, arg: &str) -> Result<(), glib::Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ffi::ostree_kernel_args_new_replace(
                self.to_glib_none_mut().0,
                arg.to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(())
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    /// Append from a whitespace-separated string.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn parse_append(&mut self, options: &str) {
        unsafe {
            ffi::ostree_kernel_args_parse_append(
                self.to_glib_none_mut().0,
                options.to_glib_none().0,
            );
        }
    }

    /// Replace a kernel argument.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace(&mut self, arg: &str) {
        unsafe {
            ffi::ostree_kernel_args_replace(self.to_glib_none_mut().0, arg.to_glib_none().0);
        }
    }

    /// Replace multiple kernel arguments.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace_argv(&mut self, argv: &[&str]) {
        unsafe {
            ffi::ostree_kernel_args_replace_argv(self.to_glib_none_mut().0, argv.to_glib_none().0);
        }
    }

    /// A duplicate of `replace`.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace_take(&mut self, arg: &str) {
        unsafe {
            ffi::ostree_kernel_args_replace_take(self.to_glib_none_mut().0, arg.to_glib_full());
        }
    }

    /// Convert the kernel arguments to a string.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    fn to_gstring(&self) -> GString {
        unsafe {
            from_glib_full(ffi::ostree_kernel_args_to_string(
                self.to_glib_none().0 as *mut OstreeKernelArgs,
            ))
        }
    }

    /// Convert the kernel arguments to a string array.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn to_strv(&self) -> Vec<GString> {
        unsafe {
            FromGlibPtrContainer::from_glib_full(ffi::ostree_kernel_args_to_strv(
                self.to_glib_none().0 as *mut OstreeKernelArgs,
            ))
        }
    }

    // Not needed
    //pub fn cleanup(loc: /*Unimplemented*/Option<Fundamental: Pointer>)

    /// Parse the given string as kernel arguments.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn from_string(options: &str) -> KernelArgs {
        unsafe {
            from_glib_full(ffi::ostree_kernel_args_from_string(
                options.to_glib_none().0,
            ))
        }
    }

    /// Create new empty kernel arguments.
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn new() -> KernelArgs {
        unsafe { from_glib_full(ffi::ostree_kernel_args_new()) }
    }
}

#[cfg(any(feature = "v2019_3", feature = "dox"))]
impl Default for KernelArgs {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Display for KernelArgs {
    #[inline]
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.to_gstring())
    }
}

impl<T: AsRef<str>> From<T> for KernelArgs {
    fn from(v: T) -> Self {
        KernelArgs::from_string(v.as_ref())
    }
}
