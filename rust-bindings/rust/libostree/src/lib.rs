extern crate libostree_sys as ffi;

extern crate glib_sys as glib_ffi;
extern crate gobject_sys as gobject_ffi;

#[macro_use]
extern crate glib;
extern crate gio;

pub use glib::Error;

mod auto;
pub use auto::*;
