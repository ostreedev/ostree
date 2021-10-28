use crate::gio;
use crate::Sysroot;
use std::path::PathBuf;

#[derive(Clone, Debug, Default)]
/// Builder object for `Sysroot`.
pub struct SysrootBuilder {
    path: Option<PathBuf>,
    mount_namespace_in_use: bool,
}

impl SysrootBuilder {
    /// Create a new builder for `Sysroot`.
    pub fn new() -> Self {
        Self::default()
    }

    /// Set the path to the sysroot location.
    pub fn path(mut self, path: Option<PathBuf>) -> Self {
        self.path = path;
        self
    }

    #[cfg(any(feature = "v2020_1", feature = "dox"))]
    #[cfg_attr(feature = "dox", doc(cfg(feature = "v2020_1")))]
    /// Set whether the logic is running in its own mount namespace.
    pub fn mount_namespace_in_use(mut self, mount_namespace_in_use: bool) -> Self {
        self.mount_namespace_in_use = mount_namespace_in_use;
        self
    }

    /// Load an existing `Sysroot` from disk, finalizing this builder.
    pub fn load(self, cancellable: Option<&gio::Cancellable>) -> Result<Sysroot, glib::Error> {
        let sysroot = self.configure_common();
        sysroot.load(cancellable)?;

        Ok(sysroot)
    }

    /// Create a new `Sysroot` on disk, finalizing this builder.
    pub fn create(self, cancellable: Option<&gio::Cancellable>) -> Result<Sysroot, glib::Error> {
        let sysroot = self.configure_common();
        sysroot.ensure_initialized(cancellable)?;
        sysroot.load(cancellable)?;

        Ok(sysroot)
    }

    /// Perform common configuration steps, returning a not-yet-fully-loaded `Sysroot`.
    fn configure_common(self) -> Sysroot {
        let sysroot = {
            let opt_file = self.path.map(|p| gio::File::for_path(p));
            Sysroot::new(opt_file.as_ref())
        };

        #[cfg(feature = "v2020_1")]
        if self.mount_namespace_in_use {
            sysroot.set_mount_namespace_in_use();
        }

        sysroot
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_sysroot_create_load_empty() {
        // Create and load an empty sysroot. Make sure it can be properly
        // inspected as empty, without panics.
        let tmpdir = tempfile::tempdir().unwrap();

        let path_created = {
            let tmp_path = Some(tmpdir.path().to_path_buf());
            let builder = SysrootBuilder::new().path(tmp_path);

            let sysroot = builder.create(gio::NONE_CANCELLABLE).unwrap();

            assert!(sysroot.fd() >= 0);
            assert_eq!(sysroot.deployments().len(), 0);
            assert_eq!(sysroot.booted_deployment(), None);
            assert_eq!(sysroot.bootversion(), 0);
            assert_eq!(sysroot.subbootversion(), 0);
            sysroot.cleanup(gio::NONE_CANCELLABLE).unwrap();

            sysroot.path().unwrap()
        };
        let path_loaded = {
            let tmp_path = Some(tmpdir.path().to_path_buf());
            let builder = SysrootBuilder::new().path(tmp_path);

            let sysroot = builder.create(gio::NONE_CANCELLABLE).unwrap();

            assert!(sysroot.fd() >= 0);
            assert_eq!(sysroot.deployments().len(), 0);
            assert_eq!(sysroot.booted_deployment(), None);
            assert_eq!(sysroot.bootversion(), 0);
            assert_eq!(sysroot.subbootversion(), 0);
            sysroot.cleanup(gio::NONE_CANCELLABLE).unwrap();

            sysroot.path().unwrap()
        };
        assert_eq!(path_created.to_string(), path_loaded.to_string());
    }
}
