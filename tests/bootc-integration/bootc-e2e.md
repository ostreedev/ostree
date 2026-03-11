# Bootc integration tests

Integration tests that run inside a [bcvk](https://github.com/bootc-dev/bcvk) VM
against a deployed ostree system. The test binary is baked into the container
image at build time and executed over SSH.

Tests are split into two tiers: `booted_test!` tests need a fully deployed
system (via `bcvk libvirt run`) while `privileged_test!` tests just need root
and can run in a faster ephemeral VM. See the module docs in
`src/tests/privileged.rs` for details.

## Running

```sh
just build                    # build the container image
just integration-container    # run all tests in a deployed VM
just integration-ephemeral    # fast path: privileged-only tests
just integration-container --exact privileged_verify_composefs  # single test
just integration-container --list                               # list tests
just integration-cleanup      # remove leftover VMs
```

Set `JUNIT_OUTPUT` to a file path to get JUnit XML results. The
`integration-container` recipe does this automatically, writing to
`target/integration-results.xml`.
