// This file was generated by gir (https://github.com/gtk-rs/gir)
// from gir-files
// DO NOT EDIT

use crate::{Deployment,SysrootSimpleWriteDeploymentFlags};
#[cfg(feature = "v2016_4")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2016_4")))]
use crate::{DeploymentUnlockedState};
#[cfg(feature = "v2017_4")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2017_4")))]
use crate::{SysrootWriteDeploymentsOpts};
#[cfg(feature = "v2017_7")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2017_7")))]
use crate::{Repo};
#[cfg(feature = "v2020_7")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2020_7")))]
use crate::{SysrootDeployTreeOpts};
use glib::{prelude::*,translate::*};
#[cfg(feature = "v2017_10")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2017_10")))]
use glib::{signal::{connect_raw, SignalHandlerId}};
use std::{boxed::Box as Box_,pin::Pin};

glib::wrapper! {
    #[doc(alias = "OstreeSysroot")]
    pub struct Sysroot(Object<ffi::OstreeSysroot>);

    match fn {
        type_ => || ffi::ostree_sysroot_get_type(),
    }
}

impl Sysroot {
    #[doc(alias = "ostree_sysroot_new")]
    pub fn new(path: Option<&impl IsA<gio::File>>) -> Sysroot {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_new(path.map(|p| p.as_ref()).to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_new_default")]
    pub fn new_default() -> Sysroot {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_new_default())
        }
    }

    #[cfg(feature = "v2023_8")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2023_8")))]
    #[doc(alias = "ostree_sysroot_change_finalization")]
    pub fn change_finalization(&self, deployment: &Deployment) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_change_finalization(self.to_glib_none().0, deployment.to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_cleanup")]
    pub fn cleanup(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_cleanup(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    //#[cfg(feature = "v2018_6")]
    //#[cfg_attr(docsrs, doc(cfg(feature = "v2018_6")))]
    //#[doc(alias = "ostree_sysroot_cleanup_prune_repo")]
    //pub fn cleanup_prune_repo(&self, options: /*Ignored*/&mut RepoPruneOptions, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(i32, i32, u64), glib::Error> {
    //    unsafe { TODO: call ffi:ostree_sysroot_cleanup_prune_repo() }
    //}

    #[cfg(feature = "v2018_5")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2018_5")))]
    #[doc(alias = "ostree_sysroot_deploy_tree")]
    pub fn deploy_tree(&self, osname: Option<&str>, revision: &str, origin: Option<&glib::KeyFile>, provided_merge_deployment: Option<&Deployment>, override_kernel_argv: &[&str], cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<Deployment, glib::Error> {
        unsafe {
            let mut out_new_deployment = std::ptr::null_mut();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deploy_tree(self.to_glib_none().0, osname.to_glib_none().0, revision.to_glib_none().0, origin.to_glib_none().0, provided_merge_deployment.to_glib_none().0, override_kernel_argv.to_glib_none().0, &mut out_new_deployment, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib_full(out_new_deployment)) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_7")))]
    #[doc(alias = "ostree_sysroot_deploy_tree_with_options")]
    pub fn deploy_tree_with_options(&self, osname: Option<&str>, revision: &str, origin: Option<&glib::KeyFile>, provided_merge_deployment: Option<&Deployment>, opts: Option<&SysrootDeployTreeOpts>, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<Deployment, glib::Error> {
        unsafe {
            let mut out_new_deployment = std::ptr::null_mut();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deploy_tree_with_options(self.to_glib_none().0, osname.to_glib_none().0, revision.to_glib_none().0, origin.to_glib_none().0, provided_merge_deployment.to_glib_none().0, mut_override(opts.to_glib_none().0), &mut out_new_deployment, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib_full(out_new_deployment)) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_deployment_set_kargs")]
    pub fn deployment_set_kargs(&self, deployment: &Deployment, new_kargs: &[&str], cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deployment_set_kargs(self.to_glib_none().0, deployment.to_glib_none().0, new_kargs.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_deployment_set_kargs_in_place")]
    pub fn deployment_set_kargs_in_place(&self, deployment: &Deployment, kargs_str: Option<&str>, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deployment_set_kargs_in_place(self.to_glib_none().0, deployment.to_glib_none().0, kargs_str.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_deployment_set_mutable")]
    pub fn deployment_set_mutable(&self, deployment: &Deployment, is_mutable: bool, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deployment_set_mutable(self.to_glib_none().0, deployment.to_glib_none().0, is_mutable.into_glib(), cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2018_3")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2018_3")))]
    #[doc(alias = "ostree_sysroot_deployment_set_pinned")]
    pub fn deployment_set_pinned(&self, deployment: &Deployment, is_pinned: bool) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deployment_set_pinned(self.to_glib_none().0, deployment.to_glib_none().0, is_pinned.into_glib(), &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2016_4")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2016_4")))]
    #[doc(alias = "ostree_sysroot_deployment_unlock")]
    pub fn deployment_unlock(&self, deployment: &Deployment, unlocked_state: DeploymentUnlockedState, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_deployment_unlock(self.to_glib_none().0, deployment.to_glib_none().0, unlocked_state.into_glib(), cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_ensure_initialized")]
    pub fn ensure_initialized(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_ensure_initialized(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_get_booted_deployment")]
    #[doc(alias = "get_booted_deployment")]
    pub fn booted_deployment(&self) -> Option<Deployment> {
        unsafe {
            from_glib_none(ffi::ostree_sysroot_get_booted_deployment(self.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_bootversion")]
    #[doc(alias = "get_bootversion")]
    pub fn bootversion(&self) -> i32 {
        unsafe {
            ffi::ostree_sysroot_get_bootversion(self.to_glib_none().0)
        }
    }

    #[doc(alias = "ostree_sysroot_get_deployment_directory")]
    #[doc(alias = "get_deployment_directory")]
    pub fn deployment_directory(&self, deployment: &Deployment) -> gio::File {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_get_deployment_directory(self.to_glib_none().0, deployment.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_deployment_dirpath")]
    #[doc(alias = "get_deployment_dirpath")]
    pub fn deployment_dirpath(&self, deployment: &Deployment) -> glib::GString {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_get_deployment_dirpath(self.to_glib_none().0, deployment.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_deployments")]
    #[doc(alias = "get_deployments")]
    pub fn deployments(&self) -> Vec<Deployment> {
        unsafe {
            FromGlibPtrContainer::from_glib_container(ffi::ostree_sysroot_get_deployments(self.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_fd")]
    #[doc(alias = "get_fd")]
    pub fn fd(&self) -> i32 {
        unsafe {
            ffi::ostree_sysroot_get_fd(self.to_glib_none().0)
        }
    }

    #[doc(alias = "ostree_sysroot_get_merge_deployment")]
    #[doc(alias = "get_merge_deployment")]
    pub fn merge_deployment(&self, osname: Option<&str>) -> Option<Deployment> {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_get_merge_deployment(self.to_glib_none().0, osname.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_path")]
    #[doc(alias = "get_path")]
    pub fn path(&self) -> gio::File {
        unsafe {
            from_glib_none(ffi::ostree_sysroot_get_path(self.to_glib_none().0))
        }
    }

    #[cfg(feature = "v2018_5")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2018_5")))]
    #[doc(alias = "ostree_sysroot_get_staged_deployment")]
    #[doc(alias = "get_staged_deployment")]
    pub fn staged_deployment(&self) -> Option<Deployment> {
        unsafe {
            from_glib_none(ffi::ostree_sysroot_get_staged_deployment(self.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_get_subbootversion")]
    #[doc(alias = "get_subbootversion")]
    pub fn subbootversion(&self) -> i32 {
        unsafe {
            ffi::ostree_sysroot_get_subbootversion(self.to_glib_none().0)
        }
    }

    #[cfg(feature = "v2016_4")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2016_4")))]
    #[doc(alias = "ostree_sysroot_init_osname")]
    pub fn init_osname(&self, osname: &str, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_init_osname(self.to_glib_none().0, osname.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_1")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_1")))]
    #[doc(alias = "ostree_sysroot_initialize")]
    pub fn initialize(&self) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_initialize(self.to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2022_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2022_7")))]
    #[doc(alias = "ostree_sysroot_initialize_with_mount_namespace")]
    pub fn initialize_with_mount_namespace(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_initialize_with_mount_namespace(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_1")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_1")))]
    #[doc(alias = "ostree_sysroot_is_booted")]
    pub fn is_booted(&self) -> bool {
        unsafe {
            from_glib(ffi::ostree_sysroot_is_booted(self.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_load")]
    pub fn load(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_load(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2016_4")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2016_4")))]
    #[doc(alias = "ostree_sysroot_load_if_changed")]
    pub fn load_if_changed(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<bool, glib::Error> {
        unsafe {
            let mut out_changed = std::mem::MaybeUninit::uninit();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_load_if_changed(self.to_glib_none().0, out_changed.as_mut_ptr(), cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib(out_changed.assume_init())) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_lock")]
    pub fn lock(&self) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_lock(self.to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_lock_async")]
    pub fn lock_async<P: FnOnce(Result<(), glib::Error>) + 'static>(&self, cancellable: Option<&impl IsA<gio::Cancellable>>, callback: P) {
        
                let main_context = glib::MainContext::ref_thread_default();
                let is_main_context_owner = main_context.is_owner();
                let has_acquired_main_context = (!is_main_context_owner)
                    .then(|| main_context.acquire().ok())
                    .flatten();
                assert!(
                    is_main_context_owner || has_acquired_main_context.is_some(),
                    "Async operations only allowed if the thread is owning the MainContext"
                );
        
        let user_data: Box_<glib::thread_guard::ThreadGuard<P>> = Box_::new(glib::thread_guard::ThreadGuard::new(callback));
        unsafe extern "C" fn lock_async_trampoline<P: FnOnce(Result<(), glib::Error>) + 'static>(_source_object: *mut glib::gobject_ffi::GObject, res: *mut gio::ffi::GAsyncResult, user_data: glib::ffi::gpointer) {
            let mut error = std::ptr::null_mut();
            let _ = ffi::ostree_sysroot_lock_finish(_source_object as *mut _, res, &mut error);
            let result = if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) };
            let callback: Box_<glib::thread_guard::ThreadGuard<P>> = Box_::from_raw(user_data as *mut _);
            let callback: P = callback.into_inner();
            callback(result);
        }
        let callback = lock_async_trampoline::<P>;
        unsafe {
            ffi::ostree_sysroot_lock_async(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, Some(callback), Box_::into_raw(user_data) as *mut _);
        }
    }

    
    pub fn lock_future(&self) -> Pin<Box_<dyn std::future::Future<Output = Result<(), glib::Error>> + 'static>> {

        Box_::pin(gio::GioFuture::new(self, move |obj, cancellable, send| {
            obj.lock_async(
                Some(cancellable),
                move |res| {
                    send.resolve(res);
                },
            );
        }))
    }

    #[doc(alias = "ostree_sysroot_origin_new_from_refspec")]
    pub fn origin_new_from_refspec(&self, refspec: &str) -> glib::KeyFile {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_origin_new_from_refspec(self.to_glib_none().0, refspec.to_glib_none().0))
        }
    }

    #[doc(alias = "ostree_sysroot_prepare_cleanup")]
    pub fn prepare_cleanup(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_prepare_cleanup(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2017_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2017_7")))]
    #[doc(alias = "ostree_sysroot_query_deployments_for")]
    pub fn query_deployments_for(&self, osname: Option<&str>) -> (Option<Deployment>, Option<Deployment>) {
        unsafe {
            let mut out_pending = std::ptr::null_mut();
            let mut out_rollback = std::ptr::null_mut();
            ffi::ostree_sysroot_query_deployments_for(self.to_glib_none().0, osname.to_glib_none().0, &mut out_pending, &mut out_rollback);
            (from_glib_full(out_pending), from_glib_full(out_rollback))
        }
    }

    #[cfg(feature = "v2017_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2017_7")))]
    #[doc(alias = "ostree_sysroot_repo")]
    pub fn repo(&self) -> Repo {
        unsafe {
            from_glib_none(ffi::ostree_sysroot_repo(self.to_glib_none().0))
        }
    }

    #[cfg(feature = "v2021_1")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2021_1")))]
    #[doc(alias = "ostree_sysroot_require_booted_deployment")]
    pub fn require_booted_deployment(&self) -> Result<Deployment, glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let ret = ffi::ostree_sysroot_require_booted_deployment(self.to_glib_none().0, &mut error);
            if error.is_null() { Ok(from_glib_none(ret)) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_1")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_1")))]
    #[doc(alias = "ostree_sysroot_set_mount_namespace_in_use")]
    pub fn set_mount_namespace_in_use(&self) {
        unsafe {
            ffi::ostree_sysroot_set_mount_namespace_in_use(self.to_glib_none().0);
        }
    }

    #[doc(alias = "ostree_sysroot_simple_write_deployment")]
    pub fn simple_write_deployment(&self, osname: Option<&str>, new_deployment: &Deployment, merge_deployment: Option<&Deployment>, flags: SysrootSimpleWriteDeploymentFlags, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_simple_write_deployment(self.to_glib_none().0, osname.to_glib_none().0, new_deployment.to_glib_none().0, merge_deployment.to_glib_none().0, flags.into_glib(), cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_7")))]
    #[doc(alias = "ostree_sysroot_stage_overlay_initrd")]
    pub fn stage_overlay_initrd(&self, fd: i32, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<glib::GString, glib::Error> {
        unsafe {
            let mut out_checksum = std::ptr::null_mut();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_stage_overlay_initrd(self.to_glib_none().0, fd, &mut out_checksum, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib_full(out_checksum)) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2018_5")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2018_5")))]
    #[doc(alias = "ostree_sysroot_stage_tree")]
    pub fn stage_tree(&self, osname: Option<&str>, revision: &str, origin: Option<&glib::KeyFile>, merge_deployment: Option<&Deployment>, override_kernel_argv: &[&str], cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<Deployment, glib::Error> {
        unsafe {
            let mut out_new_deployment = std::ptr::null_mut();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_stage_tree(self.to_glib_none().0, osname.to_glib_none().0, revision.to_glib_none().0, origin.to_glib_none().0, merge_deployment.to_glib_none().0, override_kernel_argv.to_glib_none().0, &mut out_new_deployment, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib_full(out_new_deployment)) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2020_7")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2020_7")))]
    #[doc(alias = "ostree_sysroot_stage_tree_with_options")]
    pub fn stage_tree_with_options(&self, osname: Option<&str>, revision: &str, origin: Option<&glib::KeyFile>, merge_deployment: Option<&Deployment>, opts: &SysrootDeployTreeOpts, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<Deployment, glib::Error> {
        unsafe {
            let mut out_new_deployment = std::ptr::null_mut();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_stage_tree_with_options(self.to_glib_none().0, osname.to_glib_none().0, revision.to_glib_none().0, origin.to_glib_none().0, merge_deployment.to_glib_none().0, mut_override(opts.to_glib_none().0), &mut out_new_deployment, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib_full(out_new_deployment)) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_try_lock")]
    pub fn try_lock(&self) -> Result<bool, glib::Error> {
        unsafe {
            let mut out_acquired = std::mem::MaybeUninit::uninit();
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_try_lock(self.to_glib_none().0, out_acquired.as_mut_ptr(), &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(from_glib(out_acquired.assume_init())) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_unload")]
    pub fn unload(&self) {
        unsafe {
            ffi::ostree_sysroot_unload(self.to_glib_none().0);
        }
    }

    #[doc(alias = "ostree_sysroot_unlock")]
    pub fn unlock(&self) {
        unsafe {
            ffi::ostree_sysroot_unlock(self.to_glib_none().0);
        }
    }

    #[cfg(feature = "v2023_11")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2023_11")))]
    #[doc(alias = "ostree_sysroot_update_post_copy")]
    pub fn update_post_copy(&self, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_update_post_copy(self.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_write_deployments")]
    pub fn write_deployments(&self, new_deployments: &[Deployment], cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_write_deployments(self.to_glib_none().0, new_deployments.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[cfg(feature = "v2017_4")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2017_4")))]
    #[doc(alias = "ostree_sysroot_write_deployments_with_options")]
    pub fn write_deployments_with_options(&self, new_deployments: &[Deployment], opts: &SysrootWriteDeploymentsOpts, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_write_deployments_with_options(self.to_glib_none().0, new_deployments.to_glib_none().0, mut_override(opts.to_glib_none().0), cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_write_origin_file")]
    pub fn write_origin_file(&self, deployment: &Deployment, new_origin: Option<&glib::KeyFile>, cancellable: Option<&impl IsA<gio::Cancellable>>) -> Result<(), glib::Error> {
        unsafe {
            let mut error = std::ptr::null_mut();
            let is_ok = ffi::ostree_sysroot_write_origin_file(self.to_glib_none().0, deployment.to_glib_none().0, new_origin.to_glib_none().0, cancellable.map(|p| p.as_ref()).to_glib_none().0, &mut error);
            debug_assert_eq!(is_ok == glib::ffi::GFALSE, !error.is_null());
            if error.is_null() { Ok(()) } else { Err(from_glib_full(error)) }
        }
    }

    #[doc(alias = "ostree_sysroot_get_deployment_origin_path")]
    #[doc(alias = "get_deployment_origin_path")]
    pub fn deployment_origin_path(deployment_path: &impl IsA<gio::File>) -> gio::File {
        unsafe {
            from_glib_full(ffi::ostree_sysroot_get_deployment_origin_path(deployment_path.as_ref().to_glib_none().0))
        }
    }

    #[cfg(feature = "v2017_10")]
    #[cfg_attr(docsrs, doc(cfg(feature = "v2017_10")))]
    #[doc(alias = "journal-msg")]
    pub fn connect_journal_msg<F: Fn(&Self, &str) + Send + 'static>(&self, f: F) -> SignalHandlerId {
        unsafe extern "C" fn journal_msg_trampoline<F: Fn(&Sysroot, &str) + Send + 'static>(this: *mut ffi::OstreeSysroot, msg: *mut libc::c_char, f: glib::ffi::gpointer) {
            let f: &F = &*(f as *const F);
            f(&from_glib_borrow(this), &glib::GString::from_glib_borrow(msg))
        }
        unsafe {
            let f: Box_<F> = Box_::new(f);
            connect_raw(self.as_ptr() as *mut _, b"journal-msg\0".as_ptr() as *const _,
                Some(std::mem::transmute::<_, unsafe extern "C" fn()>(journal_msg_trampoline::<F> as *const ())), Box_::into_raw(f))
        }
    }
}

unsafe impl Send for Sysroot {}
