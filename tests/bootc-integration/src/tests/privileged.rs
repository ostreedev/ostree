//! Integration tests requiring root privileges.
//!
//! Tests are split into two categories:
//!
//! * **Booted** (`booted_test!`) — need a fully deployed ostree system
//!   (composefs, sysroot, boot markers). When not root these dispatch via
//!   `bcvk libvirt run` which does a full `bootc install to-disk`.
//!
//! * **Privileged** (`privileged_test!`) — just need root and the ostree
//!   binary installed. When not root these dispatch via the much faster
//!   `bcvk ephemeral run-ssh` (no disk install).

use anyhow::{ensure, Result};
use xshell::{cmd, Shell};

use crate::integration_test;

/// How a test should be dispatched when not running as root.
enum RunMode {
    /// Needs a fully deployed ostree system (composefs, sysroot, boot markers).
    /// Uses `bcvk libvirt run` which does a full `bootc install to-disk`.
    Booted,
    /// Just needs root and the ostree binary. Can use the faster
    /// `bcvk ephemeral run-ssh` which boots the container directly.
    Privileged,
}

/// Test that needs a fully deployed/booted ostree system.
/// Dispatches via `bcvk libvirt run` when not root.
macro_rules! booted_test {
    ($fn_name:ident, $body:expr) => {
        fn $fn_name() -> Result<()> {
            if require_root(stringify!($fn_name), RunMode::Booted)?.is_some() {
                return Ok(());
            }
            $body
        }
        integration_test!($fn_name);
    };
}

/// Test that just needs root (not a full booted deployment).
/// Dispatches via `bcvk ephemeral run-ssh` when not root — much faster.
macro_rules! privileged_test {
    ($fn_name:ident, $body:expr) => {
        fn $fn_name() -> Result<()> {
            if require_root(stringify!($fn_name), RunMode::Privileged)?.is_some() {
                return Ok(());
            }
            $body
        }
        integration_test!($fn_name);
    };
}

/// Ensure we're running as root, or re-exec this test inside a VM.
///
/// If already root (e.g. inside a bcvk VM), returns `Ok(None)` and the
/// test proceeds normally.
///
/// If not root and `OSTREE_TEST_IMAGE` is set, dispatches to a VM whose
/// type depends on `mode`:
///
/// * [`RunMode::Booted`]: deploys a libvirt VM via `bcvk libvirt run`
///   (full `bootc install to-disk`), runs the test over SSH, then tears
///   down the VM.
/// * [`RunMode::Privileged`]: uses the faster `bcvk ephemeral run-ssh`
///   which boots the container directly without a disk install.
///
/// Returns `Ok(Some(()))` after the test ran remotely — the caller should
/// return immediately. Returns an error if not root and no test image is
/// configured.
fn require_root(test_name: &str, mode: RunMode) -> Result<Option<()>> {
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

    match mode {
        RunMode::Booted => {
            // Use a unique VM name per test to avoid collisions
            let vm_name = format!("ostree-test-{}", test_name.replace('_', "-"));

            // Deploy a full VM (bootc install to-disk + boot) and wait for SSH
            cmd!(
                sh,
                "{bcvk} libvirt run --name {vm_name} --replace --detach --ssh-wait {image}"
            )
            .run()?;

            // Run the test inside the deployed VM
            let result = cmd!(
                sh,
                "{bcvk} libvirt ssh {vm_name} -- ostree-bootc-integration-tests --exact {test_name}"
            )
            .run();

            // Always clean up the VM
            let _ = cmd!(sh, "{bcvk} libvirt rm --stop --force {vm_name}").run();

            // Propagate the test result
            result?;
        }
        RunMode::Privileged => {
            // Fast path: ephemeral container, no disk install needed
            cmd!(
                sh,
                "{bcvk} ephemeral run-ssh {image} -- ostree-bootc-integration-tests --exact {test_name}"
            )
            .run()?;
        }
    }

    Ok(Some(()))
}

booted_test!(privileged_verify_ostree_booted, {
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
});

booted_test!(privileged_verify_sysroot, {
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

    let has_stateroot = std::fs::read_dir(deploy_dir)?
        .any(|entry| entry.is_ok_and(|e| e.file_type().is_ok_and(|ft| ft.is_dir())));
    ensure!(
        has_stateroot,
        "no stateroot directories found under /ostree/deploy"
    );

    Ok(())
});

booted_test!(privileged_verify_composefs, {
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
});

booted_test!(privileged_verify_ostree_cli, {
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
});

booted_test!(privileged_verify_sysroot_readonly, {
    let sh = Shell::new()?;
    let options = cmd!(sh, "findmnt -n -o OPTIONS /sysroot").read()?;
    ensure!(
        options.contains("ro"),
        "/sysroot is not mounted read-only: {options}"
    );
    Ok(())
});

booted_test!(privileged_verify_ostree_run_metadata, {
    let metadata = std::fs::metadata("/run/ostree")?;
    let mode = std::os::unix::fs::PermissionsExt::mode(&metadata.permissions()) & 0o777;
    ensure!(
        mode == 0o755,
        "/run/ostree has mode {mode:#o}, expected 0o755"
    );
    Ok(())
});

booted_test!(privileged_verify_immutable_bit, {
    let sh = Shell::new()?;
    // If composefs (overlay), the immutable bit doesn't apply
    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    if fstype.trim() == "overlay" {
        return Ok(());
    }
    // https://bugzilla.redhat.com/show_bug.cgi?id=1867601
    let lsattr_out = cmd!(sh, "lsattr -d /").read()?;
    ensure!(
        lsattr_out.contains("-i-"),
        "immutable bit not set on /: {lsattr_out}"
    );
    Ok(())
});

booted_test!(privileged_verify_osinit_unshare, {
    let sh = Shell::new()?;
    // Create a test stateroot; this should not disturb /run/ostree permissions
    cmd!(sh, "ostree admin os-init ostreetestsuite").run()?;
    let metadata = std::fs::metadata("/run/ostree")?;
    let mode = std::os::unix::fs::PermissionsExt::mode(&metadata.permissions()) & 0o777;
    ensure!(
        mode == 0o755,
        "/run/ostree has mode {mode:#o} after os-init, expected 0o755"
    );
    Ok(())
});

privileged_test!(privileged_verify_nofifo, {
    let sh = Shell::new()?;
    let tmp = tempfile::tempdir()?;
    let tmpdir = tmp.path();
    let _guard = sh.push_dir(tmpdir);
    cmd!(sh, "ostree --repo=repo init --mode=archive").run()?;
    cmd!(sh, "mkdir tmproot").run()?;
    cmd!(sh, "mkfifo tmproot/afile").run()?;
    let result = cmd!(
        sh,
        "ostree --repo=repo commit -b fifotest -s 'commit fifo' --tree=dir=./tmproot"
    )
    .ignore_status()
    .output()?;
    ensure!(
        !result.status.success(),
        "committing a FIFO should have failed"
    );
    let stderr = String::from_utf8_lossy(&result.stderr);
    ensure!(
        stderr.contains("Not a regular file or symlink"),
        "expected 'Not a regular file or symlink' in stderr, got: {stderr}"
    );
    Ok(())
});

privileged_test!(privileged_verify_mtime, {
    let sh = Shell::new()?;
    let tmp = tempfile::tempdir()?;
    let tmpdir = tmp.path();
    let _guard = sh.push_dir(tmpdir);
    cmd!(sh, "ostree --repo=repo init --mode=archive").run()?;
    cmd!(sh, "mkdir tmproot").run()?;
    cmd!(sh, "bash -c 'echo afile > tmproot/afile'").run()?;
    cmd!(sh, "ostree --repo=repo commit -b test --tree=dir=tmproot")
        .ignore_stdout()
        .run()?;
    let ts_before = tmpdir.join("repo").metadata()?.modified()?;
    std::thread::sleep(std::time::Duration::from_secs(1));
    cmd!(
        sh,
        "ostree --repo=repo commit -b test -s 'bump mtime' --tree=dir=tmproot"
    )
    .ignore_stdout()
    .run()?;
    let ts_after = tmpdir.join("repo").metadata()?.modified()?;
    ensure!(
        ts_before != ts_after,
        "repo mtime did not change after second commit"
    );
    Ok(())
});

privileged_test!(privileged_verify_extensions, {
    let sh = Shell::new()?;
    let tmp = tempfile::tempdir()?;
    let tmpdir = tmp.path();
    let _guard = sh.push_dir(tmpdir);
    cmd!(sh, "ostree --repo=repo init --mode=bare").run()?;
    let extensions = tmpdir.join("repo/extensions");
    ensure!(
        extensions.exists(),
        "repo/extensions directory was not created by ostree init"
    );
    Ok(())
});

booted_test!(privileged_verify_selinux_labels, {
    let sh = Shell::new()?;

    // Check that /etc has the correct SELinux type
    let ls_output = cmd!(sh, "ls -dZ /etc").read()?;
    if !ls_output.contains(':') {
        // SELinux is likely disabled; skip this check
        return Ok(());
    }
    ensure!(
        ls_output.contains(":etc_t:"),
        "/etc does not have :etc_t: SELinux label: {ls_output}"
    );

    Ok(())
});
