extern crate gio_sys;
extern crate glib_sys;
extern crate gobject_sys;
extern crate ostree_sys;
#[macro_use]
extern crate glib;
extern crate gio;
extern crate libc;
#[macro_use]
extern crate bitflags;
#[macro_use]
extern crate lazy_static;

use glib::Error;

// re-exports
mod auto;
pub use crate::auto::functions::*;
pub use crate::auto::*;

mod repo;

#[cfg(any(feature = "v2018_6", feature = "dox"))]
mod collection_ref;
#[cfg(any(feature = "v2018_6", feature = "dox"))]
pub use crate::collection_ref::CollectionRef;

mod object_name;
pub use crate::object_name::ObjectName;

// public modules
pub mod prelude {
    pub use crate::auto::traits::*;
    pub use crate::repo::RepoExtManual;
}
