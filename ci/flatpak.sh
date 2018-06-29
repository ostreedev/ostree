#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

# Keep this pinned to avoid arbitrary change for now; it's also
# good to test building older code against newer ostree as it helps
# us notice any API breaks.
FLATPAK_TAG=0.99.2

dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh

codedir=$(pwd)

# Build ostree, but we don't install it yet
cd ${codedir}
ci/build.sh

# Build flatpak
tmpd=$(mktemp -d)
cd ${tmpd}
git clone --recursive --depth=1 -b ${FLATPAK_TAG} https://github.com/flatpak/flatpak
cd ${tmpd}/flatpak
# This is a copy of flatpak/ci/build.sh, but we can't use that as we want to install
# our built ostree over it.
pkg_install sudo which attr fuse bison \
            libubsan libasan libtsan clang python2 \
            elfutils git gettext-devel libappstream-glib-devel hicolor-icon-theme \
            /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache}
pkg_install_testing ostree-devel ostree
pkg_install_builddeps flatpak
# Now install ostree over the package version
cd ${codedir}
make install
cd -
# And build flatpak
build
# We want to capture automake results from flatpak
cleanup() {
    mv test-suite.log ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
