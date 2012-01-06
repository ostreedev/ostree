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

YOCTO_ARCH=x86
MACHINE=i686

BUILDROOT="gnomeos-3.4-${MACHINE}-${BRANCH}"
BASE="bases/yocto/${BUILDROOT}"

OSTREE_VER=$(cd $SCRIPT_SRCDIR && git describe)

BUILDDIR=$WORKDIR/tmp-eglibc

OSTREE_REPO=$WORKDIR/repo
BUILD_TAR=$BUILDDIR/deploy/images/gnomeos-contents-$BRANCH-qemu${YOCTO_ARCH}.tar.gz

BUILD_TIME=$(date -r $BUILD_TAR)

ostree --repo=${OSTREE_REPO} commit --skip-if-unchanged -s "Build from OSTree ${OSTREE_VER}" -b "${BASE}" --tree=tar=${BUILD_TAR}
ostree --repo=${OSTREE_REPO} diff "${BASE}"^ "${BASE}" || true
ostree --repo=${OSTREE_REPO} compose -s "Initial compose" -b ${BUILDROOT} ${BASE} 
