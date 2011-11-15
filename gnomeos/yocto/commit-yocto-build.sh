# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#

set -e
set -x

if test $(id -u) = 0; then
    cat <<EOF
This script should not be run as root.
EOF
    exit 1
fi

usage () {
    echo "$0 OSTREE_REPO_PATH BINARY_TAR"
    exit 1
}

OSTREE_REPO=$1
test -n "$OSTREE_REPO" || usage
shift
BUILD_TAR=$1
test -n "$BUILD_TAR" || usage
shift

tempdir=`mktemp -d tmp-commit-yocto-build.XXXXXXXXXX`
cd $tempdir
mkdir fs
cd fs
fakeroot -s ../fakeroot.db tar xf $BUILD_TAR
fakeroot -i ../fakeroot.db ostree --repo=${OSTREE_REPO} commit -s "Build (need ostree git version here)" -b gnomeos-base
rm -rf $tempdir
