extern crate libostree_sys as ffi;

extern crate glib_sys as glib_ffi;
extern crate gobject_sys as gobject_ffi;

#[macro_use]
extern crate glib;
extern crate gio;

use glib::Error;

// re-exports
mod auto;
pub use auto::*;

// public modules
pub mod prelude;
pub use prelude::*;
