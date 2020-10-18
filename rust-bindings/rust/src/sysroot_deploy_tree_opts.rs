use glib::translate::*;
use libc::c_char;
use ostree_sys::OstreeSysrootDeployTreeOpts;

pub struct SysrootDeployTreeOpts<'a> {
    pub override_kernel_argv: Option<&'a [&'a str]>,
    pub overlay_initrds: Option<&'a [&'a str]>,
}

impl<'a> Default for SysrootDeployTreeOpts<'a> {
    fn default() -> Self {
        SysrootDeployTreeOpts {
            override_kernel_argv: None,
            overlay_initrds: None,
        }
    }
}

type OptionStrSliceStorage<'a> =
    <Option<&'a [&'a str]> as ToGlibPtr<'a, *mut *mut c_char>>::Storage;

impl<'a, 'b: 'a> ToGlibPtr<'a, *const OstreeSysrootDeployTreeOpts> for SysrootDeployTreeOpts<'b> {
    type Storage = (
        Box<OstreeSysrootDeployTreeOpts>,
        OptionStrSliceStorage<'a>,
        OptionStrSliceStorage<'a>,
    );

    fn to_glib_none(&'a self) -> Stash<*const OstreeSysrootDeployTreeOpts, Self> {
        // Creating this struct from zeroed memory is fine since it's `repr(C)` and only contains
        // primitive types. Zeroing it ensures we handle the unused bytes correctly.
        // The struct needs to be boxed so the pointer we return remains valid even as the Stash is
        // moved around.
        let mut options = Box::new(unsafe { std::mem::zeroed::<OstreeSysrootDeployTreeOpts>() });
        let override_kernel_argv = self.override_kernel_argv.to_glib_none();
        let overlay_initrds = self.overlay_initrds.to_glib_none();
        options.override_kernel_argv = override_kernel_argv.0;
        options.overlay_initrds = overlay_initrds.0;
        Stash(
            options.as_ref(),
            (options, override_kernel_argv.1, overlay_initrds.1),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{ffi::CStr, ptr::null_mut};

    unsafe fn ptr_array_to_slice<'a, T>(ptr: *mut *mut T) -> &'a [*mut T] {
        let mut len = 0;
        while !(*ptr.offset(len)).is_null() {
            len += 1;
        }
        std::slice::from_raw_parts(ptr, len as usize)
    }

    unsafe fn str_ptr_array_to_vec<'a>(ptr: *mut *mut c_char) -> Vec<&'a str> {
        ptr_array_to_slice(ptr)
            .iter()
            .map(|x| CStr::from_ptr(*x).to_str().unwrap())
            .collect()
    }

    #[test]
    fn should_convert_default_options() {
        let options = SysrootDeployTreeOpts::default();
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!((*ptr).override_kernel_argv, null_mut());
            assert_eq!((*ptr).overlay_initrds, null_mut());
        }
    }

    #[test]
    fn should_convert_non_default_options() {
        let override_kernel_argv = vec!["quiet", "splash", "ro"];
        let overlay_initrds = vec!["overlay1", "overlay2"];
        let options = SysrootDeployTreeOpts {
            override_kernel_argv: Some(&override_kernel_argv),
            overlay_initrds: Some(&overlay_initrds),
        };
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!(
                str_ptr_array_to_vec((*ptr).override_kernel_argv),
                vec!["quiet", "splash", "ro"]
            );
            assert_eq!(
                str_ptr_array_to_vec((*ptr).overlay_initrds),
                vec!["overlay1", "overlay2"]
            );
        }
    }
}
