//! Tests that mostly use the API and access the booted sysroot read-only.

use std::os::unix::prelude::PermissionsExt;
use std::path::Path;

use anyhow::Result;
use ostree_ext::prelude::*;
use ostree_ext::{gio, ostree};
use xshell::cmd;

use crate::test::*;

fn skip_non_ostree_host() -> bool {
    !std::path::Path::new("/run/ostree-booted").exists()
}

pub(crate) fn itest_sysroot_ro() -> Result<()> {
    // TODO add a skipped identifier
    if skip_non_ostree_host() {
        return Ok(());
    }
    let cancellable = Some(gio::Cancellable::new());
    let sysroot = ostree::Sysroot::new_default();
    sysroot.load(cancellable.as_ref())?;
    assert!(sysroot.is_booted());

    let booted = sysroot.booted_deployment().expect("booted deployment");
    assert!(!booted.is_staged());
    let repo = sysroot.repo();

    let csum = booted.csum();
    let csum = csum.as_str();

    let (root, rev) = repo.read_commit(csum, cancellable.as_ref())?;
    assert_eq!(rev, csum);
    let root = root.downcast::<ostree::RepoFile>().expect("downcast");
    root.ensure_resolved()?;

    Ok(())
}

pub(crate) fn itest_immutable_bit() -> Result<()> {
    if skip_non_ostree_host() {
        return Ok(());
    }
    let sh = &xshell::Shell::new()?;
    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    // If we're on composefs then we're done
    if fstype.as_str() == "overlay" {
        return Ok(());
    }
    // https://bugzilla.redhat.com/show_bug.cgi?id=1867601
    cmd_has_output(sh_inline::bash_command!("lsattr -d /").unwrap(), "-i-")?;
    Ok(())
}

pub(crate) fn itest_tmpfiles() -> Result<()> {
    if skip_non_ostree_host() {
        return Ok(());
    }
    let metadata = Path::new("/run/ostree").metadata()?;
    assert_eq!(metadata.permissions().mode() & !libc::S_IFMT, 0o755);
    Ok(())
}

pub(crate) fn itest_osinit_unshare() -> Result<()> {
    if skip_non_ostree_host() {
        return Ok(());
    }
    let sh = xshell::Shell::new()?;
    cmd!(sh, "ostree admin os-init ostreetestsuite").run()?;
    let metadata = Path::new("/run/ostree").metadata()?;
    assert_eq!(metadata.permissions().mode() & !libc::S_IFMT, 0o755);
    Ok(())
}
