use crate::{Checksum, ObjectType};
#[cfg(feature = "futures")]
use futures::future;
use glib::prelude::*;
use glib::translate::*;
use glib_sys::GFALSE;
#[cfg(feature = "futures")]
use std::boxed::Box as Box_;
use std::error::Error;
use std::mem::MaybeUninit;
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
        checksum_file_error(out_csum, error, ret)
    }
}

pub fn checksum_file_async<
    P: IsA<gio::File>,
    Q: IsA<gio::Cancellable>,
    R: FnOnce(Result<Checksum, Box<dyn Error>>) + Send + 'static,
>(
    f: &P,
    objtype: ObjectType,
    io_priority: i32,
    cancellable: Option<&Q>,
    callback: R,
) {
    let user_data: Box<R> = Box::new(callback);
    unsafe extern "C" fn checksum_file_async_trampoline<
        R: FnOnce(Result<Checksum, Box<dyn Error>>) + Send + 'static,
    >(
        _source_object: *mut gobject_sys::GObject,
        res: *mut gio_sys::GAsyncResult,
        user_data: glib_sys::gpointer,
    ) {
        let mut error = ptr::null_mut();
        let mut out_csum = MaybeUninit::uninit();
        let ret = ostree_sys::ostree_checksum_file_async_finish(
            _source_object as *mut _,
            res,
            out_csum.as_mut_ptr(),
            &mut error,
        );
        let out_csum = out_csum.assume_init();
        let result = checksum_file_error(out_csum, error, ret);
        let callback: Box<R> = Box::from_raw(user_data as *mut _);
        callback(result);
    }
    let callback = checksum_file_async_trampoline::<R>;
    unsafe {
        ostree_sys::ostree_checksum_file_async(
            f.as_ref().to_glib_none().0,
            objtype.to_glib(),
            io_priority,
            cancellable.map(|p| p.as_ref()).to_glib_none().0,
            Some(callback),
            Box::into_raw(user_data) as *mut _,
        );
    }
}

#[cfg(feature = "futures")]
pub fn checksum_file_async_future<P: IsA<gio::File> + Clone + 'static>(
    f: &P,
    objtype: ObjectType,
    io_priority: i32,
) -> Box_<dyn future::Future<Output = Result<Checksum, Box<dyn Error>>> + std::marker::Unpin> {
    use fragile::Fragile;
    use gio::GioFuture;

    let f = f.clone();
    GioFuture::new(&f, move |f, send| {
        let cancellable = gio::Cancellable::new();
        let send = Fragile::new(send);
        checksum_file_async(f, objtype, io_priority, Some(&cancellable), move |res| {
            let _ = send.into_inner().send(res);
        });
        cancellable
    })
}

pub fn checksum_file_from_input<P: IsA<gio::InputStream>, Q: IsA<gio::Cancellable>>(
    file_info: &gio::FileInfo,
    xattrs: Option<&glib::Variant>,
    in_: Option<&P>,
    objtype: ObjectType,
    cancellable: Option<&Q>,
) -> Result<Checksum, Box<dyn Error>> {
    unsafe {
        let mut out_csum = ptr::null_mut();
        let mut error = ptr::null_mut();
        let ret = ostree_sys::ostree_checksum_file_from_input(
            file_info.to_glib_none().0,
            xattrs.to_glib_none().0,
            in_.map(|p| p.as_ref()).to_glib_none().0,
            objtype.to_glib(),
            &mut out_csum,
            cancellable.map(|p| p.as_ref()).to_glib_none().0,
            &mut error,
        );
        checksum_file_error(out_csum, error, ret)
    }
}

unsafe fn checksum_file_error(
    out_csum: *mut [*mut u8; 32],
    error: *mut glib_sys::GError,
    ret: i32,
) -> Result<Checksum, Box<dyn Error>> {
    if !error.is_null() {
        Err(Box::<glib::Error>::new(from_glib_full(error)))
    } else if ret == GFALSE {
        Err(Box::new(glib_bool_error!("unknown error")))
    } else {
        Ok(Checksum::from_glib_full(out_csum))
    }
}
