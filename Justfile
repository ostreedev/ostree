# Detect the os for a workaround below
osid := `. /usr/lib/os-release && echo $ID`

stream := env('STREAM', 'stream9')
build_args := "--jobs=4 --build-arg=base=quay.io/centos-bootc/centos-bootc:"+stream

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
    bcvk libvirt run --name ostree-integration-test --replace --detach --ssh-wait localhost/ostree:latest
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

# Remove any leftover integration test VMs
integration-cleanup:
    -bcvk libvirt rm --stop --force ostree-integration-test

sourcefiles := "git ls-files '**.c' '**.cxx' '**.h' '**.hpp'"
# Reformat source files
clang-format:
    {{sourcefiles}} | xargs clang-format -i

# Check source files against clang-format defaults
clang-format-check:
    {{sourcefiles}} | xargs clang-format -i --Werror --dry-run
