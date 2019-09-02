use crate::{Checksum, ObjectType};
use glib::prelude::*;
use glib::translate::*;
use glib_sys::GFALSE;
use std::error::Error;
use std::ptr;

pub fn checksum_file<P: IsA<gio::File>, Q: IsA<gio::Cancellable>>(
    f: &P,
    objtype: ObjectType,
    cancellable: Option<&Q>,
) -> Result<Checksum, Box<dyn Error>> {
    unsafe {
        let mut out_csum = ptr::null_mut();
        let mut error = ptr::null_mut();
        let ret = ostree_sys::ostree_checksum_file(
            f.as_ref().to_glib_none().0,
            objtype.to_glib(),
            &mut out_csum,
            cancellable.map(|p| p.as_ref()).to_glib_none().0,
            &mut error,
        );

        if !error.is_null() {
            Err(Box::<glib::Error>::new(from_glib_full(error)))
        } else if ret == GFALSE {
            Err(Box::new(glib_bool_error!(
                "unknown error in ostree_checksum_file"
            )))
        } else {
            Ok(Checksum::from_glib_full(out_csum))
        }
    }
}
