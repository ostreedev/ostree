# Detect the os for a workaround below
osid := `. /usr/lib/os-release && echo $ID`

# Build the container image from current sources
build:
    podman build --jobs=4 -t localhost/ostree .

build-unittest:
    podman build --jobs=4 --target build -t localhost/ostree-buildroot .

# We need a filesystem that supports O_TMPFILE right now (i.e. not overlayfs)
# or ostree hard crashes in the http code =/
unittest_args := "--pids-limit=-1 --tmpfs /var/tmp --tmpfs /tmp"

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
unitcontainer-build:
    podman build --jobs=4 --target bin-and-test -t localhost/ostree-bintest .
unitcontainer: unitcontainer-build 
    # need cap-add=all for mounting
    podman run --rm --net=none {{unitpriv}} {{unittest_args}} --cap-add=all --env=TEST_CONTAINER=1 localhost/ostree-bintest /tests/run.sh
    
# Run a build on the host system
build-host:
    . ci/libbuild.sh && build
