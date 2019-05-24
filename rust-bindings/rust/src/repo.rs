use crate::Repo;
#[cfg(any(feature = "v2016_4", feature = "dox"))]
use crate::RepoListRefsExtFlags;
use gio;
use glib;
use glib::translate::*;
use glib::Error;
use glib::IsA;
use glib_sys;
use ostree_sys;
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::ptr;
use ObjectName;

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
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
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
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
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
                cancellable.map(|p| p.as_ref()).to_glib_none().0,
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
