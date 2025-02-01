// This file was generated by gir (https://github.com/gtk-rs/gir)
// from gir-files
// DO NOT EDIT

use crate::{ffi,Repo,RepoCommitFilterResult,RepoCommitModifierFlags,SePolicy};
#[cfg(feature = "v2017_13")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2017_13")))]
use crate::{RepoDevInoCache};
use glib::{translate::*};
#[cfg(feature = "v2020_4")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2020_4")))]
use glib::{prelude::*};
use std::{boxed::Box as Box_};

glib::wrapper! {
    #[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct RepoCommitModifier(Shared<ffi::OstreeRepoCommitModifier>);

    match fn {
        ref => |ptr| ffi::ostree_repo_commit_modifier_ref(ptr),
        unref => |ptr| ffi::ostree_repo_commit_modifier_unref(ptr),
        type_ => || ffi::ostree_repo_commit_modifier_get_type(),
    }
}

impl RepoCommitModifier {
    #[doc(alias = "ostree_repo_commit_modifier_new")]
    pub fn new(flags: RepoCommitModifierFlags, commit_filter: Option<Box_<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>) -> RepoCommitModifier {
        let commit_filter_data: Box_<Option<Box_<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>> = Box_::new(commit_filter);
        unsafe extern "C" fn commit_filter_func(repo: *mut ffi::OstreeRepo, path: *const std::ffi::c_char, file_info: *mut gio::ffi::GFileInfo, user_data: glib::ffi::gpointer) -> ffi::OstreeRepoCommitFilterResult {
            let repo = from_glib_borrow(repo);
            let path: Borrowed<glib::GString> = from_glib_borrow(path);
            let file_info = from_glib_borrow(file_info);
            let callback = &*(user_data as *mut Option<Box_<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>);
            if let Some(ref callback) = *callback {
                callback(&repo, path.as_str(), &file_info)
            } else {
                panic!("cannot get closure...")
            }
            .into_glib()
        }
        let commit_filter = if commit_filter_data.is_some() { Some(commit_filter_func as _) } else { None };
        unsafe extern "C" fn destroy_notify_func(data: glib::ffi::gpointer) {
            let _callback = Box_::from_raw(data as *mut Option<Box_<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>);
        }
        let destroy_call3 = Some(destroy_notify_func as _);
        let super_callback0: Box_<Option<Box_<dyn Fn(&Repo, &str, &gio::FileInfo) -> RepoCommitFilterResult + 'static>>> = commit_filter_data;
        unsafe {
            from_glib_full(ffi::ostree_repo_commit_modifier_new(flags.into_glib(), commit_filter, Box_::into_raw(super_callback0) as *mut _, destroy_call3))
        }
    }

    #[cfg(feature = "v2017_13")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2017_13")))]
    #[doc(alias = "ostree_repo_commit_modifier_set_devino_cache")]
    pub fn set_devino_cache(&self, cache: &RepoDevInoCache) {
        unsafe {
            ffi::ostree_repo_commit_modifier_set_devino_cache(self.to_glib_none().0, cache.to_glib_none().0);
        }
    }

    #[doc(alias = "ostree_repo_commit_modifier_set_sepolicy")]
    pub fn set_sepolicy(&self, sepolicy: Option<&SePolicy>) {
        unsafe {
            ffi::ostree_repo_commit_modifier_set_sepolicy(self.to_glib_none().0, sepolicy.to_glib_none().0);
        }
    }

    #[cfg(feature = "v2020_4")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_4")))]
    #[doc(alias = "ostree_repo_commit_modifier_set_sepolicy_from_commit")]
    pub fn set_sepolicy_from_commit(&self, repo: &Repo, rev: &str, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_repo_commit_modifier_set_sepolicy_from_commit(self.to_glib_none().0, repo.to_glib_none().0, rev.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_repo_commit_modifier_set_xattr_callback")]
    pub fn set_xattr_callback<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(&self, callback: P) {
        let callback_data: Box_<P> = Box_::new(callback);
        unsafe extern "C" fn callback_func<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(repo: *mut ffi::OstreeRepo, path: *const std::ffi::c_char, file_info: *mut gio::ffi::GFileInfo, user_data: glib::ffi::gpointer) -> *mut glib::ffi::GVariant {
            let repo = from_glib_borrow(repo);
            let path: Borrowed<glib::GString> = from_glib_borrow(path);
            let file_info = from_glib_borrow(file_info);
            let callback = &*(user_data as *mut P);
            (*callback)(&repo, path.as_str(), &file_info)
            .to_glib_full()
        }
        let callback = Some(callback_func::<P> as _);
        unsafe extern "C" fn destroy_func<P: Fn(&Repo, &str, &gio::FileInfo) -> glib::Variant + 'static>(data: glib::ffi::gpointer) {
            let _callback = Box_::from_raw(data as *mut P);
        }
        let destroy_call2 = Some(destroy_func::<P> as _);
        let super_callback0: Box_<P> = callback_data;
        unsafe {
            ffi::ostree_repo_commit_modifier_set_xattr_callback(self.to_glib_none().0, callback, destroy_call2, Box_::into_raw(super_callback0) as *mut _);
        }
    }
}
