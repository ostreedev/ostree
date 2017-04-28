#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 "$@"
    make -j 8
}

codedir=$(pwd)

# Core prep
dnf -y install dnf-plugins-core
dnf install -y @buildsys-build
dnf install -y 'dnf-command(builddep)'

# build+install ostree
dnf builddep -y ostree
build
make install
tmpd=$(mktemp -d)
cd ${tmpd}
# Frozen to a tag for now on general principle
git clone --recursive --depth=1 -b 0.9.3 https://github.com/flatpak/flatpak
cd flatpak
dnf builddep -y flatpak
# And runtime deps
dnf install -y flatpak && rpm -e flatpak
dnf install which attr fuse parallel # for the test suite
build
# We want to capture automake results from flatpak
cleanup() {
    mv test-suite.log ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
