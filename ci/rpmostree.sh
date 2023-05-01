#!/bin/bash
# Build and run rpm-ostree's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

# Frozen to a tag for now to help predictability; it's
# also useful to test building *older* versions since
# that must work.
RPMOSTREE_TAG=v2019.4

dn=$(dirname $0)
. ${dn}/libbuild.sh

codedir=$(pwd)

pkg_upgrade
pkg_install_buildroot
pkg_builddep ostree rpm-ostree
pkg_install rpm-ostree && rpm -e rpm-ostree

# Duplicate of deps from ci/installdeps.sh in rpm-ostree for tests
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq python3-pyyaml \
    libubsan libasan libtsan elfutils fuse sudo python3-gobject-base \
    selinux-policy-devel selinux-policy-targeted python3-createrepo_c \
    rsync python3-rpm parallel clang rustfmt-preview clang-tools-extra

# From rpm-ostree/ci/vmcheck-provision.sh
pkg_install openssh-clients ansible

# build+install ostree
cd ${codedir}
build ${CONFIGOPTS:-}
make install

tmpd=$(mktemp -d)
cd ${tmpd}
git clone --recursive --depth=1 -b ${RPMOSTREE_TAG} https://github.com/projectatomic/rpm-ostree
cd rpm-ostree
build
# We want to capture automake results
cleanup() {
    mv test-suite.log ${codedir} || true
    mv vmcheck ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
# Basic sanity test of rpm-ostree+new ostree by restarting rpm-ostreed
if ! make vmsync; then
    ssh -o User=root vmcheck 'journalctl --no-pager | tail -1000'
    echo "vmsync failed"; exit 1
fi
# Now run tests; just a subset ⊂ for now to avoid CI overload
make vmcheck TESTS="layering-basic-1 layering-basic-2"
