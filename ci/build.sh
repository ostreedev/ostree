#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

install_builddeps ostree

dnf install -y sudo which attr fuse gjs parallel coccinelle clang \
    libubsan libasan libtsan PyYAML gnome-desktop-testing redhat-rpm-config \
    elfutils

build --enable-gtk-doc --enable-installed-tests=exclusive ${CONFIGOPTS:-}
