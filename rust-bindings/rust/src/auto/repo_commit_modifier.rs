// This file was generated by gir (https://github.com/gtk-rs/gir)
// from gir-files (https://github.com/gtk-rs/gir-files)
// DO NOT EDIT

use Repo;
use RepoCommitFilterResult;
use RepoCommitModifierFlags;
#[cfg(any(feature = "v2017_13", feature = "dox"))]
use RepoDevInoCache;
use SePolicy;
use gio;
use glib;
use glib::GString;
use glib::translate::*;
use ostree_sys;
use std::boxed::Box as Box_;

glib_wrapper! {
    #[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct RepoCommitModifier(Shared<ostree_sys::OstreeRepoCommitModifier>);

    match fn {
        ref => |ptr| ostree_sys::ostree_repo_commit_modifier_ref(ptr),
        unref => |ptr| ostree_sys::ostree_repo_commit_modifier_unref(ptr),
        get_type => || ostree_sys::ostree_repo_commit_modifier_get_type(),
    }
}

impl RepoCommitModifier {
    pub fn new(flags: RepoCommitModifierFlags, commit_filter: Option<Box<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>) -> RepoCommitModifier {
        let commit_filter_data: Box_<Option<Box<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>> = Box::new(commit_filter);
        unsafe extern "C" fn commit_filter_func(repo: *mut ostree_sys::OstreeRepo, path: *const libc::c_char, file_info: *mut gio_sys::GFileInfo, user_data: glib_sys::gpointer) -> ostree_sys::OstreeRepoCommitFilterResult {
            let repo = from_glib_borrow(repo);
            let path: GString = from_glib_borrow(path);
            let file_info = from_glib_borrow(file_info);
            let callback: &Option<Box<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>> = &*(user_data as *mut _);
            let res = if let Some(ref callback) = *callback {
                callback(&repo, path.as_str(), &file_info)
            } else {
                panic!("cannot get closure...")
            };
            res.to_glib()
        }
        let commit_filter = if commit_filter_data.is_some() { Some(commit_filter_func as _) } else { None };
        unsafe extern "C" fn destroy_notify_func(data: glib_sys::gpointer) {
            let _callback: Box_<Option<Box<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>> = Box_::from_raw(data as *mut _);
        }
        let destroy_call3 = Some(destroy_notify_func as _);
        let super_callback0: Box_<Option<Box<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>> = commit_filter_data;
        unsafe {
            from_glib_full(ostree_sys::ostree_repo_commit_modifier_new(flags.to_glib(), commit_filter, Box::into_raw(super_callback0) as *mut _, destroy_call3))
        }
    }

    #[cfg(any(feature = "v2017_13", feature = "dox"))]
    pub fn set_devino_cache(&self, cache: &RepoDevInoCache) {
        unsafe {
            ostree_sys::ostree_repo_commit_modifier_set_devino_cache(self.to_glib_none().0, cache.to_glib_none().0);
        }
    }

    pub fn set_sepolicy(&self, sepolicy: Option<&SePolicy>) {
        unsafe {
            ostree_sys::ostree_repo_commit_modifier_set_sepolicy(self.to_glib_none().0, sepolicy.to_glib_none().0);
        }
    }

    pub fn set_xattr_callback<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(&self, callback: P) {
        let callback_data: Box_<P> = Box::new(callback);
        unsafe extern "C" fn callback_func<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(repo: *mut ostree_sys::OstreeRepo, path: *const libc::c_char, file_info: *mut gio_sys::GFileInfo, user_data: glib_sys::gpointer) -> *mut glib_sys::GVariant {
            let repo = from_glib_borrow(repo);
            let path: GString = from_glib_borrow(path);
            let file_info = from_glib_borrow(file_info);
            let callback: &P = &*(user_data as *mut _);
            let res = (*callback)(&repo, path.as_str(), &file_info);
            res.to_glib_full()
        }
        let callback = Some(callback_func::<P> as _);
        unsafe extern "C" fn destroy_func<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(data: glib_sys::gpointer) {
            let _callback: Box_<P> = Box_::from_raw(data as *mut _);
        }
        let destroy_call2 = Some(destroy_func::<P> as _);
        let super_callback0: Box_<P> = callback_data;
        unsafe {
            ostree_sys::ostree_repo_commit_modifier_set_xattr_callback(self.to_glib_none().0, callback, destroy_call2, Box::into_raw(super_callback0) as *mut _);
        }
    }
}