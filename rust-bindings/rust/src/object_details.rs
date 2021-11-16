use glib;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fmt::Error;

/// Details of an object in an OSTree repo. It contains information about if
/// the object is "loose", and contains a list of pack file checksums in which
/// this object appears.
#[derive(Debug)]
pub struct ObjectDetails {
    loose: bool,
    object_appearances: Vec<String>,
}

impl ObjectDetails {
    /// Create a new `ObjectDetails` from a serialized representation.
    pub fn new_from_variant(variant: glib::Variant) -> Option<ObjectDetails> {
        let deserialize = variant.get::<(bool, Vec<String>)>()?;
        Some(ObjectDetails {
            loose: deserialize.0,
            object_appearances: deserialize.1,
        })
    }

    /// is object available "loose"
    pub fn is_loose(&self) -> bool {
        self.loose
    }

    /// Provide list of pack file checksums in which the object appears
    pub fn appearances(&self) -> &Vec<String> {
        &self.object_appearances
    }

    /// Format this `ObjectDetails` as a string.
    fn to_string(&self) -> String {
        format!("Object is {} loose and appears in {} checksums",
                if self.loose {"available"} else {"not available"},
                self.object_appearances.len() )
    }
}

impl Display for ObjectDetails{
    fn fmt(&self, f: &mut Formatter) -> Result<(), Error> {
        write!(f, "{}", self.to_string())
    }
}

