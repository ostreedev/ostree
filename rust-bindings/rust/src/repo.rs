#[cfg(any(feature = "v2016_4", feature = "dox"))]
use crate::RepoListRefsExtFlags;
use crate::{Checksum, ObjectName, ObjectDetails, ObjectType, Repo, RepoTransactionStats};
use ffi;
use ffi::OstreeRepoListObjectsFlags;
use glib::ffi as glib_sys;
use glib::{self, translate::*, Error, IsA};
use std::{
    collections::{HashMap, HashSet},
    future::Future,
    mem::MaybeUninit,
    path::Path,
    pin::Pin,
    ptr,
};

unsafe extern "C" fn read_variant_table(
    _key: glib_sys::gpointer,
    value: glib_sys::gpointer,
    hash_set: glib_sys::gpointer,
) {
    let value: glib::Variant = from_glib_none(value as *const glib_sys::GVariant);
    let set: &mut HashSet<ObjectName> = &mut *(hash_set as *mut HashSet<ObjectName>);
    set.insert(ObjectName::new_from_variant(value));
}

unsafe extern "C" fn read_variant_object_map(
    key: glib_sys::gpointer,
    value: glib_sys::gpointer,
    hash_set: glib_sys::gpointer,
) {
    let key: glib::Variant = from_glib_none(key as *const glib_sys::GVariant);
    let value: glib::Variant = from_glib_none(value as *const glib_sys::GVariant);
    let set: &mut HashMap<ObjectName, ObjectDetails> = &mut *(hash_set as *mut HashMap<ObjectName, ObjectDetails>);
    if let Some(details) = ObjectDetails::new_from_variant(value) {
        set.insert(ObjectName::new_from_variant(key), details);
    }
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

unsafe fn from_glib_container_variant_map(ptr: *mut glib_sys::GHashTable) -> HashMap<ObjectName, ObjectDetails> {
    let mut set = HashMap::new();
    glib_sys::g_hash_table_foreach(
        ptr,
        Some(read_variant_object_map),
        &mut set as *mut HashMap<ObjectName, ObjectDetails> as *mut _,
    );
    glib_sys::g_hash_table_unref(ptr);
    set
}

/// An open transaction in the repository.
///
/// This will automatically invoke [`ostree::Repo::abort_transaction`] when the value is dropped.
pub struct TransactionGuard<'a> {
    /// Reference to the repository for this transaction.
    repo: Option<&'a Repo>,
}

impl<'a> TransactionGuard<'a> {
    /// Commit this transaction.
    pub fn commit<P: IsA<gio::Cancellable>>(
        mut self,
        cancellable: Option<&P>,
    ) -> Result<RepoTransactionStats, glib::Error> {
        // Safety: This is the only function which mutates this option
        let repo = self.repo.take().unwrap();
        repo.commit_transaction(cancellable)
    }
}

impl<'a> Drop for TransactionGuard<'a> {
    fn drop(&mut self) {
        if let Some(repo) = self.repo {
            // TODO: better logging in ostree?
            // See also https://github.com/ostreedev/ostree/issues/2413
            let _ = repo.abort_transaction(gio::NONE_CANCELLABLE);
        }
    }
}

impl Repo {
    /// Create a new `Repo` object for working with an OSTree repo at the given path.
    pub fn new_for_path<P: AsRef<Path>>(path: P) -> Repo {
        Repo::new(&gio::File::for_path(path.as_ref()))
    }

    /// A wrapper for [`prepare_transaction`] which ensures the transaction will be aborted when the guard goes out of scope.
    pub fn auto_transaction<P: IsA<gio::Cancellable>>(
        &self,
        cancellable: Option<&P>,
    ) -> Result<TransactionGuard, glib::Error> {
        let _ = self.prepare_transaction(cancellable)?;
        Ok(TransactionGuard { repo: Some(self) })
    }

    /// Return a copy of the directory file descriptor for this repository.
    #[cfg(any(feature = "v2016_4", feature = "dox"))]
    #[cfg_attr(feature = "dox", doc(cfg(feature = "v2016_4")))]
    pub fn dfd_as_file(&self) -> std::io::Result<std::fs::File> {
        use std::os::unix::prelude::FromRawFd;
        use std::os::unix::prelude::IntoRawFd;
        unsafe {
            // A temporary owned file instance
            let dfd = std::fs::File::from_raw_fd(self.dfd());
            // So we can call dup() on it
            let copy = dfd.try_clone();
            // Now release our temporary ownership of the original
            let _ = dfd.into_raw_fd();
            Ok(copy?)
        }
    }

    /// Find all objects reachable from a commit.
    pub fn traverse_commit<P: IsA<gio::Cancellable>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: Option<&P>,
    ) -> Result<HashSet<ObjectName>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ffi::ostree_repo_traverse_commit(
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

    /// List all branch names (refs).
    pub fn list_refs<P: IsA<gio::Cancellable>>(
        &self,
        refspec_prefix: Option<&str>,
        cancellable: Option<&P>,
    ) -> Result<HashMap<String, String>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ffi::ostree_repo_list_refs(
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

    /// List all repo objects
    pub fn list_objects<P: IsA<gio::Cancellable>>(
        &self,
        flags: OstreeRepoListObjectsFlags,
        cancellable: Option<&P>,
    ) -> Result<HashMap<ObjectName, ObjectDetails>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();

            ffi::ostree_repo_list_objects(
                self.to_glib_none().0,
                flags,
                &mut hashtable,
                cancellable.map(AsRef::as_ref).to_glib_none().0,
                &mut error
            );

            if error.is_null() {
                Ok(from_glib_container_variant_map(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    /// List refs with extended options.
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
            let _ = ffi::ostree_repo_list_refs_ext(
                self.to_glib_none().0,
                refspec_prefix.to_glib_none().0,
                &mut hashtable,
                flags.into_glib(),
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

    /// Resolve a refspec to a commit SHA256.
    /// Returns an error if the refspec does not exist.
    pub fn require_rev(&self, refspec: &str) -> Result<glib::GString, Error> {
        // SAFETY: Since we said `false` for "allow_noent", this function must return a value
        Ok(self.resolve_rev(refspec, false)?.unwrap())
    }

    /// Write a content object from provided input.
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
            let _ = ffi::ostree_repo_write_content(
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

    /// Write a metadata object.
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
            let _ = ffi::ostree_repo_write_metadata(
                self.to_glib_none().0,
                objtype.into_glib(),
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

    /// Asynchronously write a content object.
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
            _source_object: *mut glib::gobject_ffi::GObject,
            res: *mut gio::ffi::GAsyncResult,
            user_data: glib::ffi::gpointer,
        ) {
            let mut error = ptr::null_mut();
            let mut out_csum = MaybeUninit::uninit();
            let _ = ffi::ostree_repo_write_content_finish(
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
            ffi::ostree_repo_write_content_async(
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

    /// Asynchronously write a content object.
    pub fn write_content_async_future<P: IsA<gio::InputStream> + Clone + 'static>(
        &self,
        expected_checksum: Option<&str>,
        object: &P,
        length: u64,
    ) -> Pin<Box<dyn Future<Output = Result<Checksum, Error>> + 'static>> {
        let expected_checksum = expected_checksum.map(ToOwned::to_owned);
        let object = object.clone();
        Box::pin(gio::GioFuture::new(self, move |obj, cancellable, send| {
            obj.write_content_async(
                expected_checksum
                    .as_ref()
                    .map(::std::borrow::Borrow::borrow),
                &object,
                length,
                Some(cancellable),
                move |res| {
                    send.resolve(res);
                },
            );
        }))
    }

    /// Asynchronously write a metadata object.
    pub fn write_metadata_async<
        P: IsA<gio::Cancellable>,
        Q: FnOnce(Result<Checksum, Error>) + Send + 'static,
    >(
        &self,
        objtype: ObjectType,
        expected_checksum: Option<&str>,
        object: &glib::Variant,
        cancellable: Option<&P>,
        callback: Q,
    ) {
        let user_data: Box<Q> = Box::new(callback);
        unsafe extern "C" fn write_metadata_async_trampoline<
            Q: FnOnce(Result<Checksum, Error>) + Send + 'static,
        >(
            _source_object: *mut glib::gobject_ffi::GObject,
            res: *mut gio::ffi::GAsyncResult,
            user_data: glib_sys::gpointer,
        ) {
            let mut error = ptr::null_mut();
            let mut out_csum = MaybeUninit::uninit();
            let _ = ffi::ostree_repo_write_metadata_finish(
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
            let callback: Box<Q> = Box::from_raw(user_data as *mut _);
            callback(result);
        }
        let callback = write_metadata_async_trampoline::<Q>;
        unsafe {
            ffi::ostree_repo_write_metadata_async(
                self.to_glib_none().0,
                objtype.into_glib(),
                expected_checksum.to_glib_none().0,
                object.to_glib_none().0,
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
                Some(callback),
                Box::into_raw(user_data) as *mut _,
            );
        }
    }

    /// Asynchronously write a metadata object.
    pub fn write_metadata_async_future(
        &self,
        objtype: ObjectType,
        expected_checksum: Option<&str>,
        object: &glib::Variant,
    ) -> Pin<Box<dyn Future<Output = Result<Checksum, Error>> + 'static>> {
        let expected_checksum = expected_checksum.map(ToOwned::to_owned);
        let object = object.clone();
        Box::pin(gio::GioFuture::new(self, move |obj, cancellable, send| {
            obj.write_metadata_async(
                objtype,
                expected_checksum
                    .as_ref()
                    .map(::std::borrow::Borrow::borrow),
                &object,
                Some(cancellable),
                move |res| {
                    send.resolve(res);
                },
            );
        }))
    }
}
