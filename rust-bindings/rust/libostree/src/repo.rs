use auto::Repo;
use ffi;
use gio;
use glib;
use glib::Error;
use glib::IsA;
use glib::translate::*;
use glib_ffi;
use std::collections::HashSet;
use std::ptr;


unsafe extern "C" fn read_variant_table(_key: glib_ffi::gpointer, value: glib_ffi::gpointer, hash_set: glib_ffi::gpointer) {
    let value: glib::Variant = from_glib_none(value as *const glib_ffi::GVariant);
    let set: &mut HashSet<glib::Variant> = &mut *(hash_set as *mut HashSet<glib::Variant>);
    set.insert(value);
}


unsafe fn from_glib_container_variant_set(ptr: *mut glib_ffi::GHashTable) -> HashSet<glib::Variant> {
    let mut set = HashSet::new();
    glib_ffi::g_hash_table_foreach(ptr, Some(read_variant_table), &mut set as *mut HashSet<glib::Variant> as *mut _);
    glib_ffi::g_hash_table_unref(ptr);
    set
}


pub trait RepoExtManual {
    fn new_for_str(path: &str) -> Repo;
    fn traverse_commit_manual<'a, P: Into<Option<&'a gio::Cancellable>>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: P) -> Result<HashSet<glib::Variant>, Error>;
}

impl<O: IsA<Repo> + IsA<glib::Object> + Clone + 'static> RepoExtManual for O {
    fn new_for_str(path: &str) -> Repo {
        Repo::new(&gio::File::new_for_path(path))
    }

    fn traverse_commit_manual<'a, P: Into<Option<&'a gio::Cancellable>>>(
        &self,
        commit_checksum: &str,
        maxdepth: i32,
        cancellable: P,
    ) -> Result<HashSet<glib::Variant>, Error> {
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
            if error.is_null() { Ok(from_glib_container_variant_set(hashtable)) } else { Err(from_glib_full(error)) }
        }
    }
}
