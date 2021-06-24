use glib::translate::{FromGlibPtrFull, FromGlibPtrNone};
use glib_sys::{g_free, g_malloc0, gpointer};
use once_cell::sync::OnceCell;
use std::ptr::copy_nonoverlapping;

const BYTES_LEN: usize = ffi::OSTREE_SHA256_DIGEST_LEN as usize;

static BASE64_CONFIG: OnceCell<radix64::CustomConfig> = OnceCell::new();

fn base64_config() -> &'static radix64::CustomConfig {
    BASE64_CONFIG.get_or_init(|| {
        radix64::configs::CustomConfigBuilder::with_alphabet(
            // modified base64 alphabet used by ostree (uses _ instead of /)
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+_",
        )
        .no_padding()
        .build()
        .unwrap()
    })
}

#[derive(Debug, thiserror::Error)]
pub enum ChecksumError {
    #[error("invalid hex checksum string")]
    InvalidHexString,
    #[error("invalid base64 checksum string")]
    InvalidBase64String,
}

/// A binary SHA256 checksum.
#[derive(Debug)]
pub struct Checksum {
    bytes: *mut [u8; BYTES_LEN],
}

// Safety: just a pointer to some memory owned by the type itself.
unsafe impl Send for Checksum {}

impl Checksum {
    /// Create a `Checksum` from a byte array.
    ///
    /// This copies the array.
    pub fn from_bytes(bytes: &[u8; BYTES_LEN]) -> Checksum {
        let mut checksum = Checksum::zeroed();
        checksum.as_mut().copy_from_slice(bytes);
        checksum
    }

    /// Create a `Checksum` from a hexadecimal SHA256 string.
    pub fn from_hex(hex_checksum: &str) -> Result<Checksum, ChecksumError> {
        let mut checksum = Checksum::zeroed();
        match hex::decode_to_slice(hex_checksum, checksum.as_mut()) {
            Ok(_) => Ok(checksum),
            Err(_) => Err(ChecksumError::InvalidHexString),
        }
    }

    /// Create a `Checksum` from a base64-encoded String.
    pub fn from_base64(b64_checksum: &str) -> Result<Checksum, ChecksumError> {
        let mut checksum = Checksum::zeroed();
        match base64_config().decode_slice(b64_checksum, checksum.as_mut()) {
            Ok(BYTES_LEN) => Ok(checksum),
            Ok(_) => Err(ChecksumError::InvalidBase64String),
            Err(_) => Err(ChecksumError::InvalidBase64String),
        }
    }

    /// Convert checksum to hex-encoded string.
    pub fn to_hex(&self) -> String {
        hex::encode(self.as_slice())
    }

    /// Convert checksum to base64 string.
    pub fn to_base64(&self) -> String {
        base64_config().encode(self.as_slice())
    }

    /// Create a `Checksum` value, taking ownership of the given memory location.
    ///
    /// # Safety
    /// `bytes` must point to an initialized 32-byte memory location that is freeable with
    /// `g_free` (this is e.g. the case if the memory was allocated with `g_malloc`). The returned
    /// value takes ownership of the memory and frees it on drop.
    unsafe fn new(bytes: *mut [u8; BYTES_LEN]) -> Checksum {
        assert!(!bytes.is_null());
        Checksum { bytes }
    }

    /// Create a `Checksum` value initialized to 0.
    fn zeroed() -> Checksum {
        let bytes = unsafe { g_malloc0(BYTES_LEN) as *mut [u8; BYTES_LEN] };
        Checksum { bytes }
    }

    /// Get a shared reference to the inner array.
    fn as_slice(&self) -> &[u8; BYTES_LEN] {
        unsafe { &(*self.bytes) }
    }

    /// Get a mutable reference to the inner array.
    fn as_mut(&mut self) -> &mut [u8; BYTES_LEN] {
        unsafe { &mut (*self.bytes) }
    }
}

impl Drop for Checksum {
    fn drop(&mut self) {
        unsafe {
            g_free(self.bytes as gpointer);
        }
    }
}

impl Clone for Checksum {
    fn clone(&self) -> Self {
        unsafe { Checksum::from_glib_none(self.bytes) }
    }
}

impl PartialEq for Checksum {
    fn eq(&self, other: &Self) -> bool {
        unsafe {
            let ret =
                ffi::ostree_cmp_checksum_bytes(self.bytes as *const u8, other.bytes as *const u8);
            ret == 0
        }
    }
}

impl Eq for Checksum {}

impl std::fmt::Display for Checksum {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "{}", self.to_hex())
    }
}

impl FromGlibPtrFull<*mut [u8; BYTES_LEN]> for Checksum {
    unsafe fn from_glib_full(ptr: *mut [u8; BYTES_LEN]) -> Self {
        Checksum::new(ptr)
    }
}

impl FromGlibPtrFull<*mut [*mut u8; BYTES_LEN]> for Checksum {
    unsafe fn from_glib_full(ptr: *mut [*mut u8; BYTES_LEN]) -> Self {
        Checksum::new(ptr as *mut u8 as *mut [u8; BYTES_LEN])
    }
}

impl FromGlibPtrFull<*mut u8> for Checksum {
    unsafe fn from_glib_full(ptr: *mut u8) -> Self {
        Checksum::new(ptr as *mut [u8; BYTES_LEN])
    }
}

impl FromGlibPtrNone<*mut [u8; BYTES_LEN]> for Checksum {
    unsafe fn from_glib_none(ptr: *mut [u8; BYTES_LEN]) -> Self {
        let checksum = Checksum::zeroed();
        // copy one array of BYTES_LEN elements
        copy_nonoverlapping(ptr, checksum.bytes, 1);
        checksum
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib::translate::from_glib_full;
    use glib_sys::g_malloc0;

    const CHECKSUM_BYTES: &[u8; BYTES_LEN] = b"\xbf\x87S\x06x>\xfd\xc5\xbc\xab7\xea\x10\xb6\xcaN\x9bj\xea\x8b\x94X\r\x0c\xa9J\xf1 V\\\x0e\x8a";
    const CHECKSUM_HEX: &str = "bf875306783efdc5bcab37ea10b6ca4e9b6aea8b94580d0ca94af120565c0e8a";
    const CHECKSUM_BASE64: &str = "v4dTBng+_cW8qzfqELbKTptq6ouUWA0MqUrxIFZcDoo";

    #[test]
    fn should_create_checksum_from_bytes_taking_ownership() {
        let bytes = unsafe { g_malloc0(BYTES_LEN) } as *mut u8;
        let checksum: Checksum = unsafe { from_glib_full(bytes) };
        assert_eq!(checksum.to_string(), "00".repeat(BYTES_LEN));
    }

    #[test]
    fn should_create_checksum_from_bytes() {
        let checksum = Checksum::from_bytes(CHECKSUM_BYTES);
        assert_eq!(checksum.to_hex(), CHECKSUM_HEX);
    }

    #[test]
    fn should_parse_checksum_string_to_bytes() {
        let csum = Checksum::from_hex(CHECKSUM_HEX).unwrap();
        assert_eq!(csum.to_string(), CHECKSUM_HEX);
    }

    #[test]
    fn should_fail_for_too_short_hex_string() {
        let result = Checksum::from_hex(&"FF".repeat(31));
        assert!(result.is_err());
    }

    #[test]
    fn should_convert_checksum_to_base64() {
        let csum = Checksum::from_hex(CHECKSUM_HEX).unwrap();
        assert_eq!(csum.to_base64(), CHECKSUM_BASE64);
    }

    #[test]
    fn should_convert_base64_string_to_checksum() {
        let csum = Checksum::from_base64(CHECKSUM_BASE64).unwrap();
        assert_eq!(csum.to_base64(), CHECKSUM_BASE64);
        assert_eq!(csum.to_string(), CHECKSUM_HEX);
    }

    #[test]
    fn should_fail_for_too_short_b64_string() {
        let result = Checksum::from_base64("abcdefghi");
        assert!(result.is_err());
    }

    #[test]
    fn should_fail_for_invalid_base64_string() {
        let result = Checksum::from_base64(&"\n".repeat(43));
        assert!(result.is_err());
    }

    #[test]
    fn should_compare_checksums() {
        let csum = Checksum::from_hex(CHECKSUM_HEX).unwrap();
        assert_eq!(csum, csum);
        let csum2 = Checksum::from_hex(CHECKSUM_HEX).unwrap();
        assert_eq!(csum2, csum);
    }

    #[test]
    fn should_clone_value() {
        let csum = Checksum::from_hex(CHECKSUM_HEX).unwrap();
        let csum2 = csum.clone();
        assert_eq!(csum2, csum);
        let csum3 = csum2.clone();
        assert_eq!(csum3, csum);
        assert_eq!(csum3, csum2);
    }
}
