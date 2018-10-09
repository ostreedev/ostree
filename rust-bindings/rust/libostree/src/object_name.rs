use ffi;
use functions::{object_name_deserialize, object_name_serialize, object_to_string};
use glib;
use glib::translate::*;
use glib_ffi;
use ObjectType;
use std::fmt::Display;
use std::fmt::Error;
use std::fmt::Formatter;
use std::hash::Hash;
use std::hash::Hasher;

fn hash_object_name(v: &glib::Variant) -> u32 {
    unsafe { ffi::ostree_hash_object_name(v.to_glib_none().0 as glib_ffi::gconstpointer) }
}

#[derive(PartialEq, Eq, Debug)]
pub struct ObjectName {
    variant: glib::Variant,
    checksum: String,
    object_type: ObjectType,
}

impl ObjectName {
    pub fn new_from_variant(variant: glib::Variant) -> ObjectName {
        let deserialize = object_name_deserialize(&variant);
        ObjectName {
            variant,
            checksum: deserialize.0,
            object_type: deserialize.1,
        }
    }

    pub fn new<S: Into<String>>(checksum: S, object_type: ObjectType) -> ObjectName {
        let checksum = checksum.into();
        let variant = object_name_serialize(checksum.as_str(), object_type).unwrap();
        ObjectName {
            variant,
            checksum,
            object_type,
        }
    }

    pub fn checksum(&self) -> &str {
        self.checksum.as_ref()
    }

    pub fn object_type(&self) -> ObjectType {
        self.object_type
    }

    pub fn name(&self) -> String {
        object_to_string(self.checksum(), self.object_type())
            .expect("type checks should make this safe")
    }
}

impl Display for ObjectName {
    fn fmt(&self, f: &mut Formatter) -> Result<(), Error> {
        write!(f, "{}", self.name())
    }
}

impl Hash for ObjectName {
    fn hash<H: Hasher>(&self, state: &mut H) {
        state.write_u32(hash_object_name(&self.variant));
    }
}
