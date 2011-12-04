#!/bin/bash
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#

set -e
set -x

WORKDIR=`pwd`
cd `dirname $0`
SCRIPT_SRCDIR=`pwd`
cd -

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

OSTREE_VER=$(cd $SCRIPT_SRCDIR && git describe)

BUILDDIR=$WORKDIR/tmp-eglibc

OSTREE_REPO=$WORKDIR/repo
BUILD_TAR=$BUILDDIR/deploy/images/gnomeos-contents-$BRANCH-qemu${ARCH}.tar.gz

BUILD_TIME=$(date -r $BUILD_TAR)

ostree --repo=${OSTREE_REPO} commit -s "Build from OSTree ${OSTREE_VER}" -b "gnomeos-yocto-$ARCH-$BRANCH" --tar ${BUILD_TAR}
