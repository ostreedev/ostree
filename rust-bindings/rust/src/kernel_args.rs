#[cfg(any(feature = "v2019_3", feature = "dox"))]
use gio;
#[cfg(any(feature = "v2019_3", feature = "dox"))]
use glib::object::IsA;
use glib::translate::*;
#[cfg(any(feature = "v2019_3", feature = "dox"))]
use glib::GString;
use ostree_sys;
use std::fmt;
use std::ptr;
use Error;

glib_wrapper! {
    #[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct KernelArgs(Boxed<ostree_sys::OstreeKernelArgs>);

    match fn {
        copy => |ptr| ostree_sys::ostree_kernel_args_copy(mut_override(ptr)),
        free => |ptr| ostree_sys::ostree_kernel_args_free(ptr),
    }
}

impl KernelArgs {
    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append(&mut self, arg: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_append(self.to_glib_none_mut().0, arg.to_glib_none().0);
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_argv(&mut self, argv: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_append_argv(
                self.to_glib_none_mut().0,
                argv.to_glib_none().0,
            );
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_argv_filtered(&mut self, argv: &str, prefixes: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_append_argv_filtered(
                self.to_glib_none_mut().0,
                argv.to_glib_none().0,
                prefixes.to_glib_none().0,
            );
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn append_proc_cmdline<P: IsA<gio::Cancellable>>(
        &mut self,
        cancellable: Option<&P>,
    ) -> Result<(), Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ostree_sys::ostree_kernel_args_append_proc_cmdline(
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

    pub fn delete(&mut self, arg: &str) -> Result<(), Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ostree_sys::ostree_kernel_args_delete(
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

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn delete_key_entry(&mut self, key: &str) -> Result<(), Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ostree_sys::ostree_kernel_args_delete_key_entry(
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

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn get_last_value(&mut self, key: &str) -> Option<GString> {
        unsafe {
            from_glib_none(ostree_sys::ostree_kernel_args_get_last_value(
                self.to_glib_none_mut().0,
                key.to_glib_none().0,
            ))
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn new_replace(&mut self, arg: &str) -> Result<(), Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let _ = ostree_sys::ostree_kernel_args_new_replace(
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

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn parse_append(&mut self, options: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_parse_append(
                self.to_glib_none_mut().0,
                options.to_glib_none().0,
            );
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace(&mut self, arg: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_replace(self.to_glib_none_mut().0, arg.to_glib_none().0);
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace_argv(&mut self, argv: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_replace_argv(
                self.to_glib_none_mut().0,
                argv.to_glib_none().0,
            );
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn replace_take(&mut self, arg: &str) {
        unsafe {
            ostree_sys::ostree_kernel_args_replace_take(
                self.to_glib_none_mut().0,
                arg.to_glib_full(),
            );
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    fn to_string(&mut self) -> GString {
        unsafe {
            from_glib_full(ostree_sys::ostree_kernel_args_to_string(
                self.to_glib_none_mut().0,
            ))
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn to_strv(&mut self) -> Vec<GString> {
        unsafe {
            FromGlibPtrContainer::from_glib_full(ostree_sys::ostree_kernel_args_to_strv(
                self.to_glib_none_mut().0,
            ))
        }
    }

    //#[cfg(any(feature = "v2019_3", feature = "dox"))]
    //pub fn cleanup(loc: /*Unimplemented*/Option<Fundamental: Pointer>) {
    //    unsafe { TODO: call ostree_sys:ostree_kernel_args_cleanup() }
    //}

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn from_string(options: &str) -> Option<KernelArgs> {
        unsafe {
            from_glib_full(ostree_sys::ostree_kernel_args_from_string(
                options.to_glib_none().0,
            ))
        }
    }

    #[cfg(any(feature = "v2019_3", feature = "dox"))]
    pub fn new() -> Option<KernelArgs> {
        unsafe { from_glib_full(ostree_sys::ostree_kernel_args_new()) }
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
        write!(f, "{}", self.to_string())
    }
}
