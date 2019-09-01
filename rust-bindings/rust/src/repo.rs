#[cfg(any(feature = "v2016_4", feature = "dox"))]
use crate::RepoListRefsExtFlags;
use crate::{Checksum, ObjectName, ObjectType, Repo};
#[cfg(feature = "futures")]
use futures::future;
use gio;
use gio_sys;
use glib;
use glib::translate::*;
use glib::Error;
use glib::IsA;
use glib_sys;
use ostree_sys;
use std::boxed::Box as Box_;
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::{mem::MaybeUninit, ptr};

unsafe extern "C" fn read_variant_table(
    _key: glib_sys::gpointer,
    value: glib_sys::gpointer,
    hash_set: glib_sys::gpointer,
) {
    let value: glib::Variant = from_glib_none(value as *const glib_sys::GVariant);
    let set: &mut HashSet<ObjectName> = &mut *(hash_set as *mut HashSet<ObjectName>);
    set.insert(ObjectName::new_from_variant(value));
}

unsafe fn from_glib_container_variant_set(ptr: *mut glib_sys::GHashTable) -> HashSet<ObjectName> {
    let mut set = HashSet::new();
    glib_sys::g_hash_table_foreach(
        ptr,
        Some(read_variant_table),
        &mut set as *mut HashSet<ObjectName> as *mut _,
    );
    glib_sys::g_hash_table_unref(ptr);
    set
}

impl Repo {
    /// Create a new `Repo` object for working with an OSTree repo at the given path.
    pub fn new_for_path<P: AsRef<Path>>(path: P) -> Repo {
        Repo::new(&gio::File::new_for_path(path.as_ref()))
    }

    pub fn traverse_commit<P: IsA<gio::Cancellable>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: Option<&P>,
    ) -> Result<HashSet<ObjectName>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ostree_sys::ostree_repo_traverse_commit(
                self.to_glib_none().0,
                commit_checksum.to_glib_none().0,
                maxdepth,
                &mut hashtable,
                cancellable.map(AsRef::as_ref).to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(from_glib_container_variant_set(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    pub fn list_refs<P: IsA<gio::Cancellable>>(
        &self,
        refspec_prefix: Option<&str>,
        cancellable: Option<&P>,
    ) -> Result<HashMap<String, String>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ostree_sys::ostree_repo_list_refs(
                self.to_glib_none().0,
                refspec_prefix.to_glib_none().0,
                &mut hashtable,
                cancellable.map(AsRef::as_ref).to_glib_none().0,
                &mut error,
            );

            if error.is_null() {
                Ok(FromGlibPtrContainer::from_glib_container(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    #[cfg(any(feature = "v2016_4", feature = "dox"))]
    pub fn list_refs_ext<P: IsA<gio::Cancellable>>(
        &self,
        refspec_prefix: Option<&str>,
        flags: RepoListRefsExtFlags,
        cancellable: Option<&P>,
    ) -> Result<HashMap<String, String>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ostree_sys::ostree_repo_list_refs_ext(
                self.to_glib_none().0,
                refspec_prefix.to_glib_none().0,
                &mut hashtable,
                flags.to_glib(),
                cancellable.map(AsRef::as_ref).to_glib_none().0,
                &mut error,
            );

            if error.is_null() {
                Ok(FromGlibPtrContainer::from_glib_container(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    pub fn write_content<P: IsA<gio::InputStream>, Q: IsA<gio::Cancellable>>(
        &self,
        expected_checksum: Option<&str>,
        object_input: &P,
        length: u64,
        cancellable: Option<&Q>,
    ) -> Result<Checksum, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut out_csum = ptr::null_mut();
            let _ = ostree_sys::fixed::ostree_repo_write_content(
                self.to_glib_none().0,
                expected_checksum.to_glib_none().0,
                object_input.as_ref().to_glib_none().0,
                length,
                &mut out_csum,
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(from_glib_full(out_csum))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    pub fn write_metadata<P: IsA<gio::Cancellable>>(
        &self,
        objtype: ObjectType,
        expected_checksum: Option<&str>,
        object: &glib::Variant,
        cancellable: Option<&P>,
    ) -> Result<Checksum, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut out_csum = ptr::null_mut();
            let _ = ostree_sys::fixed::ostree_repo_write_metadata(
                self.to_glib_none().0,
                objtype.to_glib(),
                expected_checksum.to_glib_none().0,
                object.to_glib_none().0,
                &mut out_csum,
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(from_glib_full(out_csum))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    pub fn write_content_async<
        P: IsA<gio::InputStream>,
        Q: IsA<gio::Cancellable>,
        R: FnOnce(Result<Checksum, Error>) + Send + 'static,
    >(
        &self,
        expected_checksum: Option<&str>,
        object: &P,
        length: u64,
        cancellable: Option<&Q>,
        callback: R,
    ) {
        let user_data: Box<R> = Box::new(callback);
        unsafe extern "C" fn write_content_async_trampoline<
            R: FnOnce(Result<Checksum, Error>) + Send + 'static,
        >(
            _source_object: *mut gobject_sys::GObject,
            res: *mut gio_sys::GAsyncResult,
            user_data: glib_sys::gpointer,
        ) {
            let mut error = ptr::null_mut();
            let mut out_csum = MaybeUninit::uninit();
            let _ = ostree_sys::ostree_repo_write_content_finish(
                _source_object as *mut _,
                res,
                out_csum.as_mut_ptr(),
                &mut error,
            );
            let out_csum = out_csum.assume_init();
            let result = if error.is_null() {
                Ok(Checksum::from_glib_full(out_csum))
            } else {
                Err(from_glib_full(error))
            };
            let callback: Box<R> = Box::from_raw(user_data as *mut _);
            callback(result);
        }
        let callback = write_content_async_trampoline::<R>;
        unsafe {
            ostree_sys::ostree_repo_write_content_async(
                self.to_glib_none().0,
                expected_checksum.to_glib_none().0,
                object.as_ref().to_glib_none().0,
                length,
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
                Some(callback),
                Box::into_raw(user_data) as *mut _,
            );
        }
    }

    #[cfg(feature = "futures")]
    pub fn write_content_async_future<P: IsA<gio::InputStream> + Clone + 'static>(
        &self,
        expected_checksum: Option<&str>,
        object: &P,
        length: u64,
    ) -> Box_<dyn future::Future<Output = Result<Checksum, Error>> + std::marker::Unpin> {
        use fragile::Fragile;
        use gio::GioFuture;

        let expected_checksum = expected_checksum.map(ToOwned::to_owned);
        let object = object.clone();
        GioFuture::new(self, move |obj, send| {
            let cancellable = gio::Cancellable::new();
            let send = Fragile::new(send);
            obj.write_content_async(
                expected_checksum
                    .as_ref()
                    .map(::std::borrow::Borrow::borrow),
                &object,
                length,
                Some(&cancellable),
                move |res| {
                    let _ = send.into_inner().send(res);
                },
            );

            cancellable
        })
    }
}
