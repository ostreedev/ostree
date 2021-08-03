use ffi::OstreeSysrootWriteDeploymentsOpts;
use glib::translate::*;

/// Options for writing a deployment.
pub struct SysrootWriteDeploymentsOpts {
    /// Perform cleanup after writing the deployment.
    pub do_postclean: bool,
}

impl Default for SysrootWriteDeploymentsOpts {
    fn default() -> Self {
        SysrootWriteDeploymentsOpts {
            do_postclean: false,
        }
    }
}

impl<'a> ToGlibPtr<'a, *const OstreeSysrootWriteDeploymentsOpts> for SysrootWriteDeploymentsOpts {
    type Storage = Box<OstreeSysrootWriteDeploymentsOpts>;

    fn to_glib_none(&'a self) -> Stash<*const OstreeSysrootWriteDeploymentsOpts, Self> {
        // Creating this struct from zeroed memory is fine since it's `repr(C)` and only contains
        // primitive types.
        // The struct needs to be boxed so the pointer we return remains valid even as the Stash is
        // moved around.
        let mut options =
            Box::new(unsafe { std::mem::zeroed::<OstreeSysrootWriteDeploymentsOpts>() });
        options.do_postclean = self.do_postclean.into_glib();
        Stash(options.as_ref(), options)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib_sys::{GFALSE, GTRUE};

    #[test]
    fn should_convert_default_options() {
        let options = SysrootWriteDeploymentsOpts::default();
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!((*ptr).do_postclean, GFALSE);
        }
    }

    #[test]
    fn should_convert_non_default_options() {
        let options = SysrootWriteDeploymentsOpts { do_postclean: true };
        let stash = options.to_glib_none();
        let ptr = stash.0;
        unsafe {
            assert_eq!((*ptr).do_postclean, GTRUE);
        }
    }
}
