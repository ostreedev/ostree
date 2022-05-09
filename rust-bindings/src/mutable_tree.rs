use crate::MutableTree;
use glib::{self, translate::*};
use std::collections::HashMap;

impl MutableTree {
    #[doc(alias = "ostree_mutable_tree_get_files")]
    /// Create a copy of the files in this mutable tree.
    /// Unlike the C version of this function, a copy is made because providing
    /// read-write access would introduce the potential for use-after-free bugs.
    pub fn copy_files(&self) -> HashMap<String, String> {
        unsafe {
            let v = ffi::ostree_mutable_tree_get_files(self.to_glib_none().0);
            HashMap::from_glib_none_num(v, 1)
        }
    }

    #[doc(alias = "ostree_mutable_tree_get_subdirs")]
    /// Create a copy of the directories in this mutable tree.
    /// Unlike the C version of this function, a copy is made because providing
    /// read-write access would introduce the potential for use-after-free bugs.
    pub fn copy_subdirs(&self) -> HashMap<String, Self> {
        use glib::ffi::gpointer;

        unsafe {
            let v = ffi::ostree_mutable_tree_get_subdirs(self.to_glib_none().0);
            unsafe extern "C" fn visit_hash_table(
                key: gpointer,
                value: gpointer,
                hash_map: gpointer,
            ) {
                let key: String = from_glib_none(key as *const libc::c_char);
                let value: MutableTree = from_glib_none(value as *const ffi::OstreeMutableTree);
                let hash_map: &mut HashMap<String, MutableTree> =
                    &mut *(hash_map as *mut HashMap<String, MutableTree>);
                hash_map.insert(key, value);
            }
            let mut map = HashMap::with_capacity(glib::ffi::g_hash_table_size(v) as usize);
            glib::ffi::g_hash_table_foreach(
                v,
                Some(visit_hash_table),
                &mut map as *mut HashMap<String, MutableTree> as *mut _,
            );
            map
        }
    }
}
