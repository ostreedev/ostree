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

    /// Finalize this builder into a `Sysroot`.
    pub fn build(self, cancellable: Option<&gio::Cancellable>) -> Result<Sysroot, glib::Error> {
        let sysroot = {
            let opt_file = self.path.map(|p| gio::File::for_path(p));
            Sysroot::new(opt_file.as_ref())
        };

        #[cfg(feature = "v2020_1")]
        if self.mount_namespace_in_use {
            sysroot.set_mount_namespace_in_use();
        }

        sysroot.load(cancellable)?;

        Ok(sysroot)
    }
}
