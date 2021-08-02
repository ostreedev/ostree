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
