// This file was generated by gir (https://github.com/gtk-rs/gir)
// from gir-files
// DO NOT EDIT

use crate::{RepoFinder};
#[cfg(feature = "v2018_6")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2018_6")))]
use glib::{translate::*};

glib::wrapper! {
    #[doc(alias = "OstreeRepoFinderConfig")]
    pub struct RepoFinderConfig(Object<ffi::OstreeRepoFinderConfig, ffi::OstreeRepoFinderConfigClass>) @implements RepoFinder;

    match fn {
        type_ => || ffi::ostree_repo_finder_config_get_type(),
    }
}

impl RepoFinderConfig {
    #[cfg(feature = "v2018_6")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2018_6")))]
    #[doc(alias = "ostree_repo_finder_config_new")]
    pub fn new() -> RepoFinderConfig {
        unsafe {
            from_glib_full(ffi::ostree_repo_finder_config_new())
        }
    }
}

#[cfg(feature = "v2018_6")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2018_6")))]
impl Default for RepoFinderConfig {
                     fn default() -> Self {
                         Self::new()
                     }
                 }
