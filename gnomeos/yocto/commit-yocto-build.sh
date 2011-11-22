# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#

set -e
set -x

WORKDIR=`pwd`

if test $(id -u) = 0; then
    cat <<EOF
This script should not be run as root.
EOF
    exit 1
fi

usage () {
    echo "$0 BRANCH"
    exit 1
}

BRANCH=$1
test -n "$BRANCH" || usage
shift

ARCH=x86

BUILDDIR=$WORKDIR/tmp-eglibc

OSTREE_REPO=$WORKDIR/repo
BUILD_TAR=$BUILDDIR/deploy/images/gnomeos-contents-$BRANCH-qemu${ARCH}.tar.gz

BUILD_TIME=$(date -r $BUILD_TAR)

tempdir=`mktemp -d tmp-commit-yocto-build.XXXXXXXXXX`
cd $tempdir
mkdir fs
cd fs
fakeroot -s ../fakeroot.db tar xf $BUILD_TAR
fakeroot -i ../fakeroot.db ostree --repo=${OSTREE_REPO} commit -s "Build ${BUILD_TIME}" -b "gnomeos-$ARCH-$BRANCH"
cd "${WORKDIR}"
rm -rf $tempdir
