#!/usr/bin/bash
# Install build dependencies.

set -xeuo pipefail

# This is used by our OpenShift Prow job; we use the
# cosa buildroot container
# https://github.com/coreos/coreos-assembler/pull/730
# And using `yum` at all means we can flake on fetching rpm metadata
if [ -n "${SKIP_INSTALLDEPS:-}" ] || test "$(id -u)" != 0; then
    exit 0
fi

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_upgrade
pkg_install_buildroot
pkg_install ccache
pkg_builddep ostree
# Not yet in the spec
pkg_install composefs-devel
pkg_install sudo which attr fuse strace \
    libubsan libasan libtsan redhat-rpm-config \
    elfutils fsverity-utils jq
if test -n "${CI_PKGS:-}"; then
    pkg_install ${CI_PKGS}
fi
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang \
                  python3-PyYAML
