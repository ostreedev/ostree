#!/usr/bin/bash
# Prepare the current environment

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh
pkg_upgrade
pkg_install_buildroot
pkg_install sudo which attr fuse strace \
            libubsan libasan libtsan PyYAML elfutils
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang

if test -n "${CI_PKGS:-}"; then
    pkg_install ${CI_PKGS}
fi
