#!/bin/bash
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
    echo "$0"
    exit 1
}

if ! test -f ostree-qemu.img; then
    echo "ostree-qemu.img not found; You must run gnomeos-qemu-create.sh first"
fi

current_uname=$(uname -r)

ARGS="$@"
if ! echo $ARGS | grep -q 'ostree='; then
    ARGS="ostree=current $ARGS"
fi
ARGS="rd.plymouth=0 root=/dev/sda $ARGS"
KERNEL=/boot/vmlinuz-${current_uname}
INITRD_ARG="-initrd /boot/initramfs-ostree-${current_uname}.img"

exec qemu-kvm -kernel ${KERNEL} ${INITRD_ARG} -hda ostree-qemu.img -net user -net nic,model=virtio -m 512M -append "$ARGS" -monitor stdio
