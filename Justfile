# Detect the os for a workaround below
osid := `. /usr/lib/os-release && echo $ID`

stream := env('STREAM', 'stream9')
build_args := "--jobs=4 --build-arg=base=quay.io/centos-bootc/centos-bootc:"+stream

# Build RPMs into target/packages/
package:
    #!/bin/bash
    set -xeuo pipefail
    packages=target/packages
    if test -n "${OSTREE_SKIP_PACKAGE:-}"; then
        if test '!' -d "${packages}"; then
            echo "OSTREE_SKIP_PACKAGE is set, but missing ${packages}" 1>&2; exit 1
        fi
        exit 0
    fi
    podman build {{build_args}} -t localhost/ostree-pkg --target=rpmbuild .
    mkdir -p "${packages}"
    rm -vf "${packages}"/*.rpm
    podman run --rm localhost/ostree-pkg tar -C /rpms/ -cf - . | tar -C "${packages}"/ -xvf -
    chmod a+rx target "${packages}"
    chmod a+r "${packages}"/*.rpm

# Build the container image from current sources
build *ARGS:
    podman build {{build_args}} -t localhost/ostree {{ARGS}} .

build-unittest *ARGS:
    podman build {{build_args}} --target build -t localhost/ostree-buildroot {{ARGS}} .

# Do a build but don't regenerate the initramfs
build-noinitramfs *ARGS:
    podman build {{build_args}} --target rootfs -t localhost/ostree {{ARGS}} .

unitcontainer-build *ARGS:
    podman build {{build_args}} --target bin-and-test -t localhost/ostree-bintest {{ARGS}} .

# We need a filesystem that supports O_TMPFILE right now (i.e. not overlayfs)
# or ostree hard crashes in the http code =/
unittest_args := "--pids-limit=-1 --tmpfs /run --tmpfs /var/tmp --tmpfs /tmp"

# Build and then run unit tests. If this fails, it will try to print
# the errors to stderr. However, the full unabridged test log can
# be found in target/unittest/test-suite.log.
unittest *ARGS: build-unittest
    rm -rf target/unittest && mkdir -p target/unittest
    podman run --net=none {{unittest_args}} --security-opt=label=disable --rm \
        -v $(pwd)/target/unittest:/run/output --env=ARTIFACTS=/run/output \
        --env=OSTREE_TEST_SKIP=known-xfail-docker \
        localhost/ostree-buildroot  ./tests/makecheck.py {{ARGS}}

# Start an interactive shell in the unittest container
unittest-shell: build-unittest
    podman run --rm -ti {{unittest_args}} "--env=PS1=unittests> " localhost/ostree-buildroot  bash

# For some reason doing the bind mount isn't working on at least the GHA Ubuntu 24.04 runner
# without --privileged. I think it may be apparmor?
unitpriv := if osid == "ubuntu" { "--privileged" } else { "" }
unitcontainer: unitcontainer-build 
    # need cap-add=all for mounting
    podman run --rm --net=none {{unitpriv}} {{unittest_args}} --cap-add=all --env=TEST_CONTAINER=1 localhost/ostree-bintest /tests/run.sh
# For iterating on the tests
unitcontainer-fast:
    podman run --rm --net=none {{unitpriv}} {{unittest_args}} --cap-add=all --env=TEST_CONTAINER=1 -v $(pwd)/tests-unit-container:/tests:ro --security-opt=label=disable localhost/ostree-bintest /tests/run.sh

# Run a build on the host system
build-host:
    . ci/libbuild.sh && build

# Run a build on the host system and "install" into target/inst
# This directory tree can then be copied elsewhere
build-host-inst: build-host
    make -C target/c install DESTDIR=$(pwd)/target/inst
    tar --sort=name --numeric-owner --owner=0 --group=0 -C target/inst -czf target/inst.tar.gz .

# Run all integration tests inside a deployed libvirt VM.
# This runs both "booted" tests (need full deployment) and "privileged"
# tests (just need root). For faster iteration on privileged-only tests,
# use `integration-ephemeral`.
# JUnit XML results are written to target/integration-results.xml.
integration-container *ARGS:
    #!/bin/bash
    set -euo pipefail
    bcvk libvirt run --name ostree-integration-test --replace --detach localhost/ostree:latest
    echo "Waiting for SSH..."
    for i in $(seq 1 30); do
        if bcvk libvirt ssh ostree-integration-test -- true 2>/dev/null; then
            echo "SSH ready after ~$((i * 10))s"
            break
        fi
        if [ "$i" = 30 ]; then
            echo "Timeout waiting for SSH" >&2
            bcvk libvirt rm --stop --force ostree-integration-test
            exit 1
        fi
        sleep 10
    done
    rc=0
    bcvk libvirt ssh ostree-integration-test -- env JUNIT_OUTPUT=/tmp/junit.xml \
        ostree-bootc-integration-tests {{ARGS}} || rc=$?
    mkdir -p target
    bcvk libvirt ssh ostree-integration-test -- cat /tmp/junit.xml \
        > target/integration-results.xml 2>/dev/null || true
    bcvk libvirt rm --stop --force ostree-integration-test
    exit $rc

# Run only root-privileged tests via an ephemeral VM (faster — no disk install).
# This skips tests that need a fully deployed/booted ostree system.
# Not all tests will pass here; use `integration-container` for the full suite.
integration-ephemeral *ARGS:
    bcvk ephemeral run-ssh localhost/ostree:latest -- ostree-bootc-integration-tests {{ARGS}}

# Run TMT tests inside bcvk-deployed VMs.
# Each plan runs in its own VM for isolation, following the
# bootc-dev/bootc cargo xtask run-tmt pattern.
test-tmt *ARGS: build
    cargo run --manifest-path tests/xtask/Cargo.toml -- run-tmt {{ARGS}}

# Remove any leftover integration test VMs
integration-cleanup:
    #!/bin/bash
    bcvk libvirt rm --stop --force ostree-integration-test 2>/dev/null || true
    bcvk libvirt list --format json 2>/dev/null | jq -r '.[].name' | grep '^ostree-tmt-' | while read vm; do
        bcvk libvirt rm --stop --force "$vm" 2>/dev/null || true
    done

sourcefiles := "git ls-files '**.c' '**.cxx' '**.h' '**.hpp'"
# Reformat source files
clang-format:
    {{sourcefiles}} | xargs clang-format -i

# Check source files against clang-format defaults
clang-format-check:
    {{sourcefiles}} | xargs clang-format -i --Werror --dry-run

clippy_config := "-A clippy::all -D clippy::correctness -D clippy::suspicious -Dunused_imports -Ddead_code"
cargo_project_features := env('CARGO_PROJECT_FEATURES', 'v2022_6')

# Run all Rust lint and format checks (mirrors CI)
validate:
    just cargo-fmt-check
    just cargo-clippy

# Check Rust formatting across all crates
cargo-fmt-check:
    cargo fmt -p ostree -- --check -l
    for d in tests/inst tests/bootc-integration tests/xtask; do cargo fmt --manifest-path $d/Cargo.toml -- --check -l; done

# Run cargo clippy across all crates
cargo-clippy:
    cargo clippy -p ostree --features={{cargo_project_features}} -- {{clippy_config}}
    for d in tests/inst tests/bootc-integration tests/xtask; do cargo clippy --manifest-path $d/Cargo.toml -- {{clippy_config}}; done
