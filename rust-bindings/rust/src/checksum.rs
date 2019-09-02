use glib::translate::{from_glib_full, FromGlibPtrFull, ToGlibPtr};
use glib::GString;
use glib_sys::{g_free, gpointer};
use std::fmt;

const BYTES_BUF_SIZE: usize = ostree_sys::OSTREE_SHA256_DIGEST_LEN as usize;
const HEX_BUF_SIZE: usize = (ostree_sys::OSTREE_SHA256_STRING_LEN + 1) as usize;
const B64_BUF_SIZE: usize = 44;

/// A binary SHA256 checksum.
#[derive(Debug)]
pub struct Checksum {
    bytes: *mut [u8; BYTES_BUF_SIZE],
}

impl Checksum {
    /// Create a `Checksum` value, taking ownership of the given memory location.
    ///
    /// # Safety
    /// `bytes` must point to a fully initialized 32-byte memory location that is freeable with
    /// `g_free` (this is e.g. the case if the memory was allocated with `g_malloc`). The value
    /// takes ownership of the memory, i.e. the memory is freed when the value is dropped. The
    /// memory must not be freed by other code.
    unsafe fn new(bytes: *mut [u8; BYTES_BUF_SIZE]) -> Checksum {
        assert!(!bytes.is_null());
        Checksum { bytes }
    }

    /// Create a `Checksum` from a hexadecimal SHA256 string.
    ///
    /// Unfortunately, the underlying libostree function has no way to report parsing errors. If the
    /// string is not a valid SHA256 string, the program will abort!
    // TODO: implement by hand to avoid stupid assertions?
    pub fn from_string(checksum: &str) -> Checksum {
        unsafe {
            from_glib_full(ostree_sys::ostree_checksum_to_bytes(
                checksum.to_glib_none().0,
            ))
        }
    }

    /// Return checksum as a hex digest string.
    fn to_gstring(&self) -> GString {
        unsafe { from_glib_full(ostree_sys::ostree_checksum_from_bytes(self.bytes)) }
    }

    /// Convert checksum to base64.
    pub fn to_base64(&self) -> String {
        let mut buf: Vec<u8> = Vec::with_capacity(B64_BUF_SIZE);
        unsafe {
            ostree_sys::ostree_checksum_b64_inplace_from_bytes(
                self.bytes,
                buf.as_mut_ptr() as *mut i8,
            );
            let len = libc::strlen(buf.as_mut_ptr() as *const i8);
            // Assumption: (len + 1) valid bytes are in the buffer.
            buf.set_len(len);
            // Assumption: all characters are ASCII, ergo valid UTF-8.
            String::from_utf8_unchecked(buf)
        }
    }
}

impl Drop for Checksum {
    fn drop(&mut self) {
        unsafe {
            g_free(self.bytes as gpointer);
        }
    }
}

impl FromGlibPtrFull<*mut [u8; BYTES_BUF_SIZE]> for Checksum {
    unsafe fn from_glib_full(ptr: *mut [u8; BYTES_BUF_SIZE]) -> Self {
        Checksum::new(ptr)
    }
}

impl FromGlibPtrFull<*mut [*mut u8; BYTES_BUF_SIZE]> for Checksum {
    unsafe fn from_glib_full(ptr: *mut [*mut u8; BYTES_BUF_SIZE]) -> Self {
        Checksum::new(ptr as *mut u8 as *mut [u8; BYTES_BUF_SIZE])
    }
}

impl FromGlibPtrFull<*mut u8> for Checksum {
    unsafe fn from_glib_full(ptr: *mut u8) -> Self {
        Checksum::new(ptr as *mut [u8; BYTES_BUF_SIZE])
    }
}

impl fmt::Display for Checksum {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.to_gstring())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib_sys::g_malloc0;

    const CHECKSUM_STRING: &str =
        "bf875306783efdc5bcab37ea10b6ca4e9b6aea8b94580d0ca94af120565c0e8a";

    #[test]
    fn should_create_checksum_from_bytes() {
        let bytes = unsafe { g_malloc0(BYTES_BUF_SIZE) } as *mut u8;
        let checksum: Checksum = unsafe { from_glib_full(bytes) };
        assert_eq!(checksum.to_string(), "00".repeat(BYTES_BUF_SIZE));
    }

    #[test]
    fn should_parse_checksum_string_to_bytes() {
        let csum = Checksum::from_string(CHECKSUM_STRING);
        assert_eq!(csum.to_string(), CHECKSUM_STRING);
    }

    #[test]
    fn should_convert_checksum_to_base64() {
        let csum = Checksum::from_string(CHECKSUM_STRING);
        assert_eq!(
            csum.to_base64(),
            "v4dTBng+_cW8qzfqELbKTptq6ouUWA0MqUrxIFZcDoo"
        );
    }
}
