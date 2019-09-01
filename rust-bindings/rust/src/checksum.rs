use glib::translate::{from_glib_full, FromGlibPtrFull};
use glib::GString;
use glib_sys::{g_free, gpointer};
use std::fmt;

pub struct Checksum {
    bytes: *mut [u8; 32],
}

impl Checksum {
    pub(crate) unsafe fn new(bytes: *mut [u8; 32]) -> Checksum {
        assert!(!bytes.is_null());
        Checksum { bytes }
    }

    fn to_gstring(&self) -> GString {
        unsafe { from_glib_full(ostree_sys::ostree_checksum_from_bytes(self.bytes)) }
    }
}

impl Drop for Checksum {
    fn drop(&mut self) {
        unsafe {
            g_free(self.bytes as gpointer);
        }
    }
}

impl FromGlibPtrFull<*mut [u8; 32]> for Checksum {
    unsafe fn from_glib_full(ptr: *mut [u8; 32]) -> Self {
        Checksum::new(ptr)
    }
}

impl FromGlibPtrFull<*mut u8> for Checksum {
    unsafe fn from_glib_full(ptr: *mut u8) -> Self {
        Checksum::new(ptr as *mut [u8; 32])
    }
}

impl fmt::Display for Checksum {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.to_gstring())
    }
}
