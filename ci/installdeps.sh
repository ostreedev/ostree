#!/usr/bin/bash
# Install build dependencies.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh

pkg_upgrade
pkg_install_buildroot
pkg_builddep ostree
pkg_install sudo which attr fuse strace \
    libubsan libasan libtsan PyYAML redhat-rpm-config \
    elfutils
if test -n "${CI_PKGS:-}"; then
    pkg_install ${CI_PKGS}
fi
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang \
                  python3-PyYAML
if test "${OS_ID}" = "centos"; then
    rpm -Uvh https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
    pkg_install python34{,-PyYAML}
fi
