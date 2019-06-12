use crate::{Repo, RepoCheckoutFilterResult};
use glib::translate::{FromGlibPtrNone, ToGlib};
use glib_sys::gpointer;
use libc::c_char;
use ostree_sys::{OstreeRepo, OstreeRepoCheckoutFilterResult};
use std::path::{Path, PathBuf};

pub type RepoCheckoutFilter = Box<dyn Fn(&Repo, &Path, &libc::stat) -> RepoCheckoutFilterResult>;

unsafe extern "C" fn filter_trampoline(
    repo: *mut OstreeRepo,
    path: *const c_char,
    stat: *mut libc::stat,
    user_data: gpointer,
) -> OstreeRepoCheckoutFilterResult {
    // TODO: handle unwinding
    let closure = user_data as *const RepoCheckoutFilter;
    let repo = FromGlibPtrNone::from_glib_none(repo);
    let path: PathBuf = FromGlibPtrNone::from_glib_none(path);
    let result = (*closure)(&repo, &path, &*stat);
    result.to_glib()
}

pub(super) fn trampoline() -> Option<
    unsafe extern "C" fn(
        *mut OstreeRepo,
        *const c_char,
        *mut libc::stat,
        gpointer,
    ) -> OstreeRepoCheckoutFilterResult,
> {
    Some(filter_trampoline)
}
