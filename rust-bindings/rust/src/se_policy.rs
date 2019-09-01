use crate::SePolicy;
use std::ptr;

impl SePolicy {
    pub fn fscreatecon_cleanup() {
        unsafe {
            ostree_sys::ostree_sepolicy_fscreatecon_cleanup(ptr::null_mut());
        }
    }
}
