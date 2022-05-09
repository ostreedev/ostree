use crate::{auto::CommitSizesEntry, auto::ObjectType};
use glib::{
    translate::{FromGlib, FromGlibPtrNone, ToGlibPtr},
    GString,
};

impl CommitSizesEntry {
    /// Object checksum as hex string.
    pub fn checksum(&self) -> GString {
        let underlying = self.to_glib_none();
        unsafe { GString::from_glib_none((*underlying.0).checksum) }
    }

    /// The object type.
    pub fn objtype(&self) -> ObjectType {
        let underlying = self.to_glib_none();
        unsafe { ObjectType::from_glib((*underlying.0).objtype) }
    }

    /// Unpacked object size.
    pub fn unpacked(&self) -> u64 {
        let underlying = self.to_glib_none();
        unsafe { (*underlying.0).unpacked }
    }

    /// Compressed object size.
    pub fn archived(&self) -> u64 {
        let underlying = self.to_glib_none();
        unsafe { (*underlying.0).archived }
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
