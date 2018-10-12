extern crate gio_sys as gio_ffi;
extern crate glib_sys as glib_ffi;
extern crate gobject_sys as gobject_ffi;
extern crate libostree_sys as ffi;
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
pub use auto::functions::*;
pub use auto::*;

mod repo;

mod object_name;
pub use object_name::ObjectName;

// public modules
pub mod prelude {
    pub use auto::traits::*;
    pub use repo::RepoExtManual;
}
