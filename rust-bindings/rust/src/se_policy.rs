use crate::SePolicy;
use std::ptr;

impl SePolicy {
    pub fn fscreatecon_cleanup() {
        unsafe {
            ffi::ostree_sepolicy_fscreatecon_cleanup(ptr::null_mut());
        }
    }
}
