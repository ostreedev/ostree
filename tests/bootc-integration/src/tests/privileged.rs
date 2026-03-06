//! Privileged integration tests for a booted ostree system.
//!
//! These tests verify that an ostree-based system booted correctly.
//! They require root and a real booted ostree deployment (e.g. inside
//! a bcvk ephemeral VM).
//!
//! When run on the host (not as root), each test automatically re-executes
//! itself inside a bcvk ephemeral VM booted from the container image.
//! The `OSTREE_IN_VM` env var prevents infinite recursion.

use anyhow::{ensure, Result};
use xshell::{cmd, Shell};

use crate::integration_test;

/// Ensure we're running as root, or re-exec this test inside a VM.
///
/// If already root (e.g. inside a bcvk VM), returns `Ok(None)` and the
/// test proceeds normally.
///
/// If not root and `OSTREE_TEST_IMAGE` is set, spawns
/// `bcvk ephemeral run-ssh <image> -- ostree-bootc-integration-tests --exact <test>`
/// and returns `Ok(Some(()))` — the caller should return immediately since
/// the test already ran in the VM.
///
/// If not root and no test image is configured, returns an error.
fn require_privileged(test_name: &str) -> Result<Option<()>> {
    if rustix::process::getuid().is_root() {
        return Ok(None);
    }

    // We're on the host without root — delegate to a VM.
    if std::env::var_os("OSTREE_IN_VM").is_some() {
        anyhow::bail!("OSTREE_IN_VM is set but we're not root — VM setup is broken");
    }

    let image = std::env::var("OSTREE_TEST_IMAGE").map_err(|_| {
        anyhow::anyhow!(
            "not root and OSTREE_TEST_IMAGE not set; \
             run `just integration-container` to build and test"
        )
    })?;

    let sh = Shell::new()?;
    let bcvk = std::env::var("BCVK_PATH").unwrap_or_else(|_| "bcvk".into());
    cmd!(
        sh,
        "{bcvk} ephemeral run-ssh {image} -- ostree-bootc-integration-tests --exact {test_name}"
    )
    .run()?;
    Ok(Some(()))
}

fn privileged_verify_ostree_booted() -> Result<()> {
    if require_privileged("privileged_verify_ostree_booted")?.is_some() {
        return Ok(());
    }

    let sh = Shell::new()?;

    // /run/ostree-booted is the canonical marker
    ensure!(
        std::path::Path::new("/run/ostree-booted").exists(),
        "/run/ostree-booted does not exist — system was not booted via ostree"
    );

    // ostree admin status should succeed and show a deployment
    let status = cmd!(sh, "ostree admin status").read()?;
    ensure!(
        !status.is_empty(),
        "ostree admin status returned empty output"
    );

    Ok(())
}
integration_test!(privileged_verify_ostree_booted);

fn privileged_verify_sysroot() -> Result<()> {
    if require_privileged("privileged_verify_sysroot")?.is_some() {
        return Ok(());
    }

    ensure!(
        std::path::Path::new("/sysroot").is_dir(),
        "/sysroot does not exist"
    );
    ensure!(
        std::path::Path::new("/ostree").is_dir(),
        "/ostree does not exist"
    );
    ensure!(
        std::path::Path::new("/ostree/repo").is_dir(),
        "/ostree/repo does not exist"
    );

    // Verify there's at least one deployment
    let deploy_dir = std::path::Path::new("/ostree/deploy");
    ensure!(deploy_dir.is_dir(), "/ostree/deploy does not exist");

    let has_stateroot = std::fs::read_dir(deploy_dir)?.any(|entry| {
        entry
            .ok()
            .map(|e| e.file_type().ok().map(|ft| ft.is_dir()).unwrap_or(false))
            .unwrap_or(false)
    });
    ensure!(
        has_stateroot,
        "no stateroot directories found under /ostree/deploy"
    );

    Ok(())
}
integration_test!(privileged_verify_sysroot);

fn privileged_verify_composefs() -> Result<()> {
    if require_privileged("privileged_verify_composefs")?.is_some() {
        return Ok(());
    }

    let sh = Shell::new()?;

    // With composefs enabled, the root filesystem should be an overlay
    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    ensure!(
        fstype.trim() == "overlay",
        "expected root filesystem type 'overlay' (composefs), got '{}'",
        fstype.trim()
    );

    // The composefs private directory should exist with restricted permissions
    let private_dir = std::path::Path::new("/run/ostree/.private");
    ensure!(
        private_dir.exists(),
        "/run/ostree/.private does not exist — composefs may not be active"
    );

    let metadata = std::fs::metadata(private_dir)?;
    let mode = std::os::unix::fs::PermissionsExt::mode(&metadata.permissions()) & 0o777;
    ensure!(
        mode == 0,
        "/run/ostree/.private has mode {mode:#o}, expected 0o000"
    );

    Ok(())
}
integration_test!(privileged_verify_composefs);

fn privileged_verify_ostree_cli() -> Result<()> {
    if require_privileged("privileged_verify_ostree_cli")?.is_some() {
        return Ok(());
    }

    let sh = Shell::new()?;

    // Verify ostree binary works and reports a version
    let version_output = cmd!(sh, "ostree --version").read()?;
    ensure!(
        version_output.contains("libostree"),
        "ostree --version output doesn't mention libostree: {version_output}"
    );

    // ostree admin status should succeed
    let status = cmd!(sh, "ostree admin status").read()?;
    ensure!(
        !status.is_empty(),
        "ostree admin status returned empty output"
    );

    // ostree refs should succeed (may be empty, but shouldn't error)
    cmd!(sh, "ostree refs --repo=/ostree/repo").run()?;

    Ok(())
}
integration_test!(privileged_verify_ostree_cli);

fn privileged_verify_selinux_labels() -> Result<()> {
    if require_privileged("privileged_verify_selinux_labels")?.is_some() {
        return Ok(());
    }

    let sh = Shell::new()?;

    // Check that /etc has the correct SELinux type
    let ls_output = cmd!(sh, "ls -dZ /etc").read()?;
    ensure!(
        ls_output.contains(":etc_t:"),
        "/etc does not have :etc_t: SELinux label: {ls_output}"
    );

    Ok(())
}
integration_test!(privileged_verify_selinux_labels);
