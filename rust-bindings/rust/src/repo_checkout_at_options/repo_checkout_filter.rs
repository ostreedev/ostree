use crate::{Repo, RepoCheckoutFilterResult};
use glib::translate::{FromGlibPtrNone, ToGlib};
use glib_sys::gpointer;
use libc::c_char;
use ostree_sys::{OstreeRepo, OstreeRepoCheckoutFilterResult};
use std::path::{Path, PathBuf};

pub type RepoCheckoutFilter =
    Option<Box<dyn Fn(&Repo, &Path, &libc::stat) -> RepoCheckoutFilterResult>>;

pub(super) unsafe extern "C" fn filter_trampoline(
    repo: *mut OstreeRepo,
    path: *const c_char,
    stat: *mut libc::stat,
    user_data: gpointer,
) -> OstreeRepoCheckoutFilterResult {
    // TODO: handle unwinding
    let closure =
        user_data as *const Box<dyn Fn(&Repo, &Path, &libc::stat) -> RepoCheckoutFilterResult>;
    let repo = FromGlibPtrNone::from_glib_none(repo);
    let path: PathBuf = FromGlibPtrNone::from_glib_none(path);
    let result = (*closure)(&repo, &path, &*stat);
    result.to_glib()
}
