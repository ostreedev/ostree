# Bootc E2E Integration Tests

This directory contains an integration test binary that is baked into the bootable
container image and run inside a [bcvk](https://github.com/bootc-dev/bcvk) ephemeral
VM. The tests verify that an ostree-based system booted correctly — composefs,
sysroot layout, SELinux labels, and core CLI functionality.

## How it works

The Dockerfile has an `integration-build` stage that compiles this Rust crate
and copies the resulting binary into the final bootable image at
`/usr/bin/ostree-bootc-integration-tests`. When CI (or a developer) runs
`just integration-container`, bcvk boots the image in an ephemeral QEMU VM
and executes the test binary over SSH.

All tests run as root inside the VM. The `require_privileged()` guard in each
test provides a developer convenience: if you run the binary directly on your
workstation (not as root), it will auto-dispatch to a bcvk VM using the
`OSTREE_TEST_IMAGE` environment variable.

## Running

```sh
# Build the image and run all integration tests
just build
just integration-container

# Run a single test
just integration-container --exact privileged_verify_composefs

# List available tests
just integration-container --list
```

The CI workflow (`.github/workflows/bootc.yaml`) runs this for both
`stream9` and `stream10` CentOS bootc base images.

## Test inventory

**Booted system verification:**
- `privileged_verify_ostree_booted` — `/run/ostree-booted` exists, `ostree admin status` works
- `privileged_verify_sysroot` — `/sysroot`, `/ostree/repo`, `/ostree/deploy` exist with a stateroot
- `privileged_verify_sysroot_readonly` — `/sysroot` is mounted read-only
- `privileged_verify_composefs` — root is an overlay mount, `/run/ostree/.private` has mode 0
- `privileged_verify_ostree_run_metadata` — `/run/ostree` has mode 0755
- `privileged_verify_immutable_bit` — immutable bit set on `/` (non-composefs only)
- `privileged_verify_selinux_labels` — `/etc` has `:etc_t:` SELinux label

**ostree CLI tests** (run in temp directories inside the VM):
- `privileged_verify_ostree_cli` — `ostree --version`, `ostree admin status`, `ostree refs`
- `privileged_verify_osinit_unshare` — `ostree admin os-init` doesn't break `/run/ostree` perms
- `privileged_verify_nofifo` — committing a FIFO to a repo fails with the expected error
- `privileged_verify_mtime` — repo directory mtime changes after a new commit
- `privileged_verify_extensions` — `ostree init --mode=bare` creates `repo/extensions/`

## Architecture

The test framework uses [libtest-mimic](https://crates.io/crates/libtest-mimic)
with [linkme](https://crates.io/crates/linkme) distributed slices for test
registration. The `integration_test!` macro registers each function into a global
slice that the `main()` harness collects into libtest-mimic trials.

This crate is a standalone workspace (not part of the root ostree workspace) because
it is compiled inside the container image, not on the host.

## Not yet ported

The following tests from the old `tests/inst` crate require multi-reboot
orchestration or heavy dependencies and are not yet implemented here:

- **Transactional upgrade interruption** — randomly kills/reboots during
  `rpm-ostree upgrade` and verifies consistency. Needs `bcvk libvirt`
  (persistent disk) for multi-reboot support.
- **Composefs signing** — enables signed composefs, reboots, verifies
  signature validation, then disables and reboots again. Same multi-reboot
  constraint.
- **HTTP pull with basic auth** — needs an in-process HTTP server.
