use crate::ObjectType;
use crate::{object_name_deserialize, object_name_serialize, object_to_string};
use glib;
use glib::translate::*;
use glib::GString;
use glib_sys;
use std::fmt::Display;
use std::fmt::Error;
use std::fmt::Formatter;
use std::hash::Hash;
use std::hash::Hasher;

fn hash_object_name(v: &glib::Variant) -> u32 {
    unsafe { ffi::ostree_hash_object_name(v.to_glib_none().0 as glib_sys::gconstpointer) }
}

/// A reference to an object in an OSTree repo. It contains both a checksum and an
/// [ObjectType](enum.ObjectType.html) which together identify an object in a repository.
#[derive(Eq, Debug)]
pub struct ObjectName {
    variant: glib::Variant,
    checksum: GString,
    object_type: ObjectType,
}

impl ObjectName {
    /// Create a new `ObjectName` from a serialized representation.
    pub fn new_from_variant(variant: glib::Variant) -> ObjectName {
        let deserialize = object_name_deserialize(&variant);
        ObjectName {
            variant,
            checksum: deserialize.0,
            object_type: deserialize.1,
        }
    }

    /// Create a new `ObjectName` with the given checksum and `ObjectType`.
    pub fn new<S: Into<GString>>(checksum: S, object_type: ObjectType) -> ObjectName {
        let checksum = checksum.into();
        let variant = object_name_serialize(checksum.as_str(), object_type)
            .expect("type checks should make this safe");
        ObjectName {
            variant,
            checksum,
            object_type,
        }
    }

    /// Return a reference to this `ObjectName`'s checksum string.
    pub fn checksum(&self) -> &str {
        self.checksum.as_str()
    }

    /// Return this `ObjectName`'s `ObjectType`.
    pub fn object_type(&self) -> ObjectType {
        self.object_type
    }

    /// Format this `ObjectName` as a string.
    fn to_string(&self) -> GString {
        object_to_string(self.checksum(), self.object_type())
            .expect("type checks should make this safe")
    }
}

impl Display for ObjectName {
    fn fmt(&self, f: &mut Formatter) -> Result<(), Error> {
        write!(f, "{}", self.to_string())
    }
}

impl Hash for ObjectName {
    fn hash<H: Hasher>(&self, state: &mut H) {
        state.write_u32(hash_object_name(&self.variant));
    }
}

impl PartialEq for ObjectName {
    fn eq(&self, other: &ObjectName) -> bool {
        self.checksum == other.checksum && self.object_type == other.object_type
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn should_stringify_object_name() {
        let object_name = ObjectName::new("abcdef123456", ObjectType::DirTree);
        let stringified = format!("{}", object_name);
        assert_eq!(stringified, "abcdef123456.dirtree");
    }

    #[test]
    fn same_values_should_be_equal() {
        let a = ObjectName::new("abc123", ObjectType::File);
        let b = ObjectName::new("abc123", ObjectType::File);
        assert_eq!(a, b);
    }

    #[test]
    fn different_values_should_not_be_equal() {
        let a = ObjectName::new("abc123", ObjectType::Commit);
        let b = ObjectName::new("abc123", ObjectType::File);
        assert_ne!(a, b);
    }

    #[test]
    fn should_create_object_name_from_variant() {
        let object_name = ObjectName::new("123456", ObjectType::CommitMeta);
        let from_variant = ObjectName::new_from_variant(object_name.variant.clone());
        assert_eq!(object_name, from_variant);
        assert_eq!("123456", from_variant.checksum());
        assert_eq!(ObjectType::CommitMeta, from_variant.object_type());
    }
}
