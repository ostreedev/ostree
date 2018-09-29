extern crate libostree_sys as ffi;
extern crate glib_sys as glib_ffi;
extern crate gobject_sys as gobject_ffi;
extern crate gio_sys as gio_ffi;
#[macro_use]
extern crate glib;
extern crate gio;
extern crate libc;
#[macro_use]
extern crate bitflags;

use glib::Error;

// re-exports
mod auto;
pub use auto::*;

mod repo;

// public modules
pub mod prelude {
    pub use auto::traits::*;
    pub use repo::RepoExtManual;
}
