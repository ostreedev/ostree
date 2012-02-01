#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Run built image in QEMU 
#
# Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

ARCH=i686
BRANCH_PREFIX="gnomeos-3.4-${ARCH}-"

OBJ=gnomeos-fs.img
if (! test -f ${OBJ}); then
    rm -f ${OBJ}.tmp
    qemu-img create ${OBJ}.tmp 6G
    mkfs.ext3 -q -F ${OBJ}.tmp
    mv ${OBJ}.tmp ${OBJ}
fi

mkdir -p fs
umount fs || true
sleep 1 # Avoid Linux kernel bug, pretty sure it's the new RCU pathname lookup
mount -o loop gnomeos-fs.img fs

cd fs

TOPROOT_BIND_MOUNTS="home root tmp"

for d in $TOPROOT_BIND_MOUNTS; do
    if ! test -d $d; then
        mkdir -m 0755 $d
    fi
done
chmod a=rwxt tmp

if ! test -d ostree; then
    mkdir ostree
    mkdir -p -m 0755 ./ostree/var/{log,run,tmp,spool}
    mkdir -p ./ostree/var/lib/dbus
    dbus-uuidgen > ./ostree/var/lib/dbus/machine-id

    mkdir ./ostree/var/lib/gdm
    chown 2:2 ./ostree/var/lib/gdm

    touch ./ostree/var/shadow
    chmod 0600 ./ostree/var/shadow

    mkdir ostree/repo
    ostree --repo=ostree/repo init

    cat >ostree/var/passwd << EOF
root::0:0:root:/:/bin/sh
dbus:*:1:1:dbus:/:/bin/false
gdm:*:2:2:gdm:/var/lib/gdm:/bin/false
EOF
    cat >ostree/var/group << EOF
root:*:0:root
dbus:*:1:
gdm:*:2:
EOF
fi

cd ostree
ostree --repo=${OSTREE_REPO} local-clone repo ${BRANCH_PREFIX}runtime ${BRANCH_PREFIX}devel
for branch in runtime devel; do
    rev=$(ostree --repo=repo rev-parse ${BRANCH_PREFIX}${branch});
    if ! test -d ${BRANCH_PREFIX}${branch}-${rev}; then
        ostree --repo=repo checkout ${rev} ${BRANCH_PREFIX}${branch}-${rev}
        ostbuild chroot-run-triggers ${BRANCH_PREFIX}${branch}-${rev}
    fi
    rm -f ${BRANCH_PREFIX}${branch}-current
    ln -s ${BRANCH_PREFIX}${branch}-${rev} ${BRANCH_PREFIX}${branch}-current
done
cd ..

test -d proc || mkdir proc # needed for ostree-init
cp -a ./ostree/${BRANCH_PREFIX}${branch}-current/usr/sbin/ostree-init .

cd ${WORKDIR}

sync
umount fs
rmdir fs

ARGS="$@"
if ! echo $ARGS | grep -q 'init='; then
    ARGS="init=/ostree-init $ARGS"
fi
if ! echo $ARGS | grep -q 'root='; then
    ARGS="root=/dev/hda $ARGS"
fi
if ! echo $ARGS | grep -q 'ostree='; then
    ARGS="ostree=${BRANCH_PREFIX}runtime-current $ARGS"
fi

exec qemu-kvm -kernel ./tmp-eglibc/deploy/images/bzImage-qemux86.bin -hda gnomeos-fs.img -net user -net nic,model=virtio -append "$ARGS" -monitor stdio
