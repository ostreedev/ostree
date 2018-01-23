#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

# Frozen to a tag for now to help predictability; it's
# also useful to test building *older* versions since
# that must work.
FLATPAK_TAG=0.10.2.1

dn=$(dirname $0)
. ${dn}/libbuild.sh

codedir=$(pwd)

pkg_upgrade
pkg_install_builddeps ostree
pkg_install_builddeps flatpak
pkg_install gettext-devel # A new dependency
# Copy of builddeps from build.sh in flatpak
pkg_install sudo which attr fuse \
            libubsan libasan libtsan \
            elfutils git gettext-devel libappstream-glib-devel \
            /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache} \
            hicolor-icon-theme
pkg_install flatpak && rpm -e flatpak

# Build and install ostree
cd ${codedir}
build
make install
tmpd=$(mktemp -d)
cd ${tmpd}
# Frozen to a tag for now on general principle
git clone --recursive --depth=1 -b ${FLATPAK_TAG} https://github.com/flatpak/flatpak
cd ${tmpd}/flatpak
build
# We want to capture automake results from flatpak
cleanup() {
    mv test-suite.log ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
