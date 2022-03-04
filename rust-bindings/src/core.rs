//! Hand written bindings for ostree-core.h

use glib::VariantDict;

/// The type of a commit object: `(a{sv}aya(say)sstayay)`
pub type CommitVariantType = (
    VariantDict,
    Vec<u8>,
    Vec<(String, Vec<u8>)>,
    String,
    String,
    u64,
    Vec<u8>,
    Vec<u8>,
);

/// The type of a dirtree object: `(a(say)a(sayay))`
pub type TreeVariantType = (Vec<(String, Vec<u8>)>, Vec<(String, Vec<u8>, Vec<u8>)>);

/// The type of a directory metadata object: `(uuua(ayay))`
pub type DirmetaVariantType = (u32, u32, u32, Vec<(Vec<u8>, Vec<u8>)>);

/// Parsed representation of directory metadata.
pub struct DirMetaParsed {
    /// The user ID.
    pub uid: u32,
    /// The group ID.
    pub gid: u32,
    /// The Unix mode, including file type flag.
    pub mode: u32,
    /// Extended attributes.
    pub xattrs: Vec<(Vec<u8>, Vec<u8>)>,
}

impl DirMetaParsed {
    /// Parse a directory metadata variant; must be of type `(uuua(ayay))`.
    pub fn from_variant(
        v: &glib::Variant,
    ) -> Result<DirMetaParsed, glib::variant::VariantTypeMismatchError> {
        let (uid, gid, mode, xattrs) = v.try_get::<crate::DirmetaVariantType>()?;
        Ok(DirMetaParsed {
            uid: u32::from_be(uid),
            gid: u32::from_be(gid),
            mode: u32::from_be(mode),
            xattrs,
        })
    }
}
