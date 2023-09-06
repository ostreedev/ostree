use crate::{auto::CommitSizesEntry, auto::ObjectType};
use glib::{
    translate::{FromGlib, FromGlibPtrNone, ToGlibPtr},
    GString,
};

impl CommitSizesEntry {
    /// Object checksum as hex string.
    pub fn checksum(&self) -> GString {
        let inner = ToGlibPtr::<*const ffi::OstreeCommitSizesEntry>::to_glib_none(self).0;
        unsafe { GString::from_glib_none((*inner).checksum) }
    }

    /// The object type.
    pub fn objtype(&self) -> ObjectType {
        let inner = ToGlibPtr::<*const ffi::OstreeCommitSizesEntry>::to_glib_none(self).0;
        unsafe { ObjectType::from_glib((*inner).objtype) }
    }

    /// Unpacked object size.
    pub fn unpacked(&self) -> u64 {
        let inner = ToGlibPtr::<*const ffi::OstreeCommitSizesEntry>::to_glib_none(self).0;
        unsafe { (*inner).unpacked }
    }

    /// Compressed object size.
    pub fn archived(&self) -> u64 {
        let inner = ToGlibPtr::<*const ffi::OstreeCommitSizesEntry>::to_glib_none(self).0;
        unsafe { (*inner).archived }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CHECKSUM_STRING: &str =
        "bf875306783efdc5bcab37ea10b6ca4e9b6aea8b94580d0ca94af120565c0e8a";

    #[test]
    fn should_get_values_from_commit_sizes_entry() {
        let entry = CommitSizesEntry::new(CHECKSUM_STRING, ObjectType::Commit, 15, 16).unwrap();
        assert_eq!(entry.checksum(), CHECKSUM_STRING);
        assert_eq!(entry.objtype(), ObjectType::Commit);
        assert_eq!(entry.unpacked(), 15);
        assert_eq!(entry.archived(), 16);
    }
}
