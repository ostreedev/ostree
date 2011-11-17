#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Run built image in QEMU 
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e
set -x

SRCDIR=`dirname $0`
WORKDIR=`pwd`

if test $(id -u) != 0; then
    cat <<EOF
This script should be run as root.
EOF
    exit 1
fi

usage () {
    echo "$0 OSTREE_REPO_PATH"
    exit 1
}

OSTREE_REPO=$1
shift
test -n "$OSTREE_REPO" || usage

OBJ=gnomeos-fs.img
if (! test -f ${OBJ}); then
    rm -f ${OBJ}.tmp
    qemu-img create ${OBJ}.tmp 2G
    mkfs.ext3 -q -F ${OBJ}.tmp
    mkdir -p fs
    umount fs || true
    mount -o loop ${OBJ}.tmp fs

    cd fs

    TOPROOT_BIND_MOUNTS="home root tmp"
    
    for d in $TOPROOT_BIND_MOUNTS; do
        mkdir -m 0755 $d
    done
    chmod a=rwxt tmp

    mkdir ostree
    mkdir -p -m 0755 ./ostree/var/{log,run,tmp,spool}
    cd ostree
    mkdir repo
    rev=$(ostree --repo=${OSTREE_REPO} rev-parse gnomeos-base);
    ostree --repo=${OSTREE_REPO} checkout ${rev} gnomeos-base-${rev}
    ln -s gnomeos-base-${rev} current
    cd ..

    mkdir proc # needed for ostree-init
    cp -a ./ostree/current/usr/sbin/ostree-init .

    cd ${WORKDIR}
    
    umount fs
    rmdir fs
    mv ${OBJ}.tmp ${OBJ}
fi

ARGS="$@"
if ! echo $ARGS | grep -q 'init='; then
    ARGS="init=/ostree-init $ARGS"
fi
if ! echo $ARGS | grep -q 'root='; then
    ARGS="root=/dev/hda $ARGS"
fi
if ! echo $ARGS | grep -q 'ostree='; then
    ARGS="ostree=current $ARGS"
fi

exec qemu-kvm -kernel ./tmp-eglibc/deploy/images/bzImage-qemux86.bin -hda gnomeos-fs.img -append "$ARGS"
