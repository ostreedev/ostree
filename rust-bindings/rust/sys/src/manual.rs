pub use libc::stat;

pub mod fixed {
    use crate::{OstreeObjectType, OstreeRepo};
    use glib::gboolean;
    use libc::c_char;

    extern "C" {
        pub fn ostree_repo_write_content(
            self_: *mut OstreeRepo,
            expected_checksum: *const c_char,
            object_input: *mut gio::GInputStream,
            length: u64,
            out_csum: *mut *mut [u8; 32],
            cancellable: *mut gio::GCancellable,
            error: *mut *mut glib::GError,
        ) -> gboolean;

        pub fn ostree_repo_write_metadata(
            self_: *mut OstreeRepo,
            objtype: OstreeObjectType,
            expected_checksum: *const c_char,
            object: *mut glib::GVariant,
            out_csum: *mut *mut [u8; 32],
            cancellable: *mut gio::GCancellable,
            error: *mut *mut glib::GError,
        ) -> gboolean;
    }
}
