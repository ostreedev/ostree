use auto::{Repo, RepoListRefsExtFlags};
use ffi;
use gio;
use glib;
use glib::translate::*;
use glib::Error;
use glib::IsA;
use glib_ffi;
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::ptr;
use ObjectName;

unsafe extern "C" fn read_variant_table(
    _key: glib_ffi::gpointer,
    value: glib_ffi::gpointer,
    hash_set: glib_ffi::gpointer,
) {
    let value: glib::Variant = from_glib_none(value as *const glib_ffi::GVariant);
    let set: &mut HashSet<ObjectName> = &mut *(hash_set as *mut HashSet<ObjectName>);
    set.insert(ObjectName::new_from_variant(value));
}

unsafe fn from_glib_container_variant_set(ptr: *mut glib_ffi::GHashTable) -> HashSet<ObjectName> {
    let mut set = HashSet::new();
    glib_ffi::g_hash_table_foreach(
        ptr,
        Some(read_variant_table),
        &mut set as *mut HashSet<ObjectName> as *mut _,
    );
    glib_ffi::g_hash_table_unref(ptr);
    set
}

pub trait RepoExtManual {
    fn new_for_path<P: AsRef<Path>>(path: P) -> Repo;

    /// Create a new set `out_reachable` containing all objects reachable
    /// from `commit_checksum`, traversing `maxdepth` parent commits.
    /// ## `commit_checksum`
    /// ASCII SHA256 checksum
    /// ## `maxdepth`
    /// Traverse this many parent commits, -1 for unlimited
    /// ## `out_reachable`
    /// Set of reachable objects
    /// ## `cancellable`
    /// Cancellable
    /// Create a new set `out_reachable` containing all objects reachable
    /// from `commit_checksum`, traversing `maxdepth` parent commits.
    /// ## `commit_checksum`
    /// ASCII SHA256 checksum
    /// ## `maxdepth`
    /// Traverse this many parent commits, -1 for unlimited
    /// ## `out_reachable`
    /// Set of reachable objects
    /// ## `cancellable`
    /// Cancellable
    fn traverse_commit<'a, P: Into<Option<&'a gio::Cancellable>>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: P,
    ) -> Result<HashSet<ObjectName>, Error>;

    /// If `refspec_prefix` is `None`, list all local and remote refspecs,
    /// with their current values in `out_all_refs`. Otherwise, only list
    /// refspecs which have `refspec_prefix` as a prefix.
    ///
    /// `out_all_refs` will be returned as a mapping from refspecs (including the
    /// remote name) to checksums. If `refspec_prefix` is non-`None`, it will be
    /// removed as a prefix from the hash table keys.
    /// ## `refspec_prefix`
    /// Only list refs which match this prefix
    /// ## `out_all_refs`
    ///
    ///  Mapping from refspec to checksum
    /// ## `cancellable`
    /// Cancellable
    /// If `refspec_prefix` is `None`, list all local and remote refspecs,
    /// with their current values in `out_all_refs`. Otherwise, only list
    /// refspecs which have `refspec_prefix` as a prefix.
    ///
    /// `out_all_refs` will be returned as a mapping from refspecs (including the
    /// remote name) to checksums. If `refspec_prefix` is non-`None`, it will be
    /// removed as a prefix from the hash table keys.
    /// ## `refspec_prefix`
    /// Only list refs which match this prefix
    /// ## `out_all_refs`
    ///
    ///  Mapping from refspec to checksum
    /// ## `cancellable`
    /// Cancellable
    fn list_refs<'a, 'b, P: Into<Option<&'a str>>, Q: Into<Option<&'b gio::Cancellable>>>(
        &self,
        refspec_prefix: P,
        cancellable: Q,
    ) -> Result<HashMap<String, String>, Error>;

    /// If `refspec_prefix` is `None`, list all local and remote refspecs,
    /// with their current values in `out_all_refs`. Otherwise, only list
    /// refspecs which have `refspec_prefix` as a prefix.
    ///
    /// `out_all_refs` will be returned as a mapping from refspecs (including the
    /// remote name) to checksums. Differently from `RepoExt::list_refs`, the
    /// `refspec_prefix` will not be removed from the refspecs in the hash table.
    /// ## `refspec_prefix`
    /// Only list refs which match this prefix
    /// ## `out_all_refs`
    ///
    ///  Mapping from refspec to checksum
    /// ## `flags`
    /// Options controlling listing behavior
    /// ## `cancellable`
    /// Cancellable
    /// If `refspec_prefix` is `None`, list all local and remote refspecs,
    /// with their current values in `out_all_refs`. Otherwise, only list
    /// refspecs which have `refspec_prefix` as a prefix.
    ///
    /// `out_all_refs` will be returned as a mapping from refspecs (including the
    /// remote name) to checksums. Differently from `RepoExt::list_refs`, the
    /// `refspec_prefix` will not be removed from the refspecs in the hash table.
    /// ## `refspec_prefix`
    /// Only list refs which match this prefix
    /// ## `out_all_refs`
    ///
    ///  Mapping from refspec to checksum
    /// ## `flags`
    /// Options controlling listing behavior
    /// ## `cancellable`
    /// Cancellable
    fn list_refs_ext<'a, 'b, P: Into<Option<&'a str>>, Q: Into<Option<&'b gio::Cancellable>>>(
        &self,
        refspec_prefix: P,
        flags: RepoListRefsExtFlags,
        cancellable: Q,
    ) -> Result<HashMap<String, String>, Error>;
}

impl<O: IsA<Repo> + IsA<glib::Object> + Clone + 'static> RepoExtManual for O {
    fn new_for_path<P: AsRef<Path>>(path: P) -> Repo {
        Repo::new(&gio::File::new_for_path(path.as_ref()))
    }

    fn traverse_commit<'a, P: Into<Option<&'a gio::Cancellable>>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: P,
    ) -> Result<HashSet<ObjectName>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ffi::ostree_repo_traverse_commit(
                self.to_glib_none().0,
                commit_checksum.to_glib_none().0,
                maxdepth,
                &mut hashtable,
                cancellable.into().to_glib_none().0,
                &mut error,
            );
            if error.is_null() {
                Ok(from_glib_container_variant_set(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    fn list_refs<'a, 'b, P: Into<Option<&'a str>>, Q: Into<Option<&'b gio::Cancellable>>>(
        &self,
        refspec_prefix: P,
        cancellable: Q,
    ) -> Result<HashMap<String, String>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ffi::ostree_repo_list_refs(
                self.to_glib_none().0,
                refspec_prefix.into().to_glib_none().0,
                &mut hashtable,
                cancellable.into().to_glib_none().0,
                &mut error,
            );

            if error.is_null() {
                Ok(FromGlibPtrContainer::from_glib_container(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }

    fn list_refs_ext<'a, 'b, P: Into<Option<&'a str>>, Q: Into<Option<&'b gio::Cancellable>>>(
        &self,
        refspec_prefix: P,
        flags: RepoListRefsExtFlags,
        cancellable: Q,
    ) -> Result<HashMap<String, String>, Error> {
        unsafe {
            let mut error = ptr::null_mut();
            let mut hashtable = ptr::null_mut();
            let _ = ffi::ostree_repo_list_refs_ext(
                self.to_glib_none().0,
                refspec_prefix.into().to_glib_none().0,
                &mut hashtable,
                flags.to_glib(),
                cancellable.into().to_glib_none().0,
                &mut error,
            );

            if error.is_null() {
                Ok(FromGlibPtrContainer::from_glib_container(hashtable))
            } else {
                Err(from_glib_full(error))
            }
        }
    }
}
