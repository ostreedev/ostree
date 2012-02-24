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

ARCH=i686
BRANCH_PREFIX="gnomeos-3.4-${ARCH}-"

OBJ=gnomeos-fs.img
if ! test -f ${OBJ}; then
    cat <<EOF
Create gnomeos-fs.img like this:
$ qemu-img create gnomeos-fs.img 6G
$ mkfs.ext4 -q -F gnomeos-fs.img
EOF
    exit 1
fi

if ! test -d ${WORKDIR}/repo/objects; then
    cat <<EOF
No ./repo/objects found
EOF
    exit 1
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
    mkdir -p ostree

    $SRCDIR/ostree-setup.sh $(pwd)/ostree
fi

rsync -a -H -v ${WORKDIR}/repo ${WORKDIR}/current ${WORKDIR}/modules ${WORKDIR}/gnomeos-3.4-* ./ostree

current_uname=$(uname -r)

cd ${WORKDIR}

sync
umount fs
rmdir fs

ARGS="$@"
if ! echo $ARGS | grep -q 'ostree='; then
    ARGS="ostree=current $ARGS"
fi
ARGS="rd.plymouth=0 root=/dev/sda $ARGS"
KERNEL=/boot/vmlinuz-${current_uname}
INITRD_ARG="-initrd /boot/initramfs-ostree-${current_uname}.img"

exec qemu-kvm -kernel ${KERNEL} ${INITRD_ARG} -hda gnomeos-fs.img -net user -net nic,model=virtio -m 512M -append "$ARGS" -monitor stdio
