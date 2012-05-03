#!/bin/bash
# -*- indent-tabs-mode: nil; -*-
# Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
#
# Prepare an empty OSTree setup on system; this presently uses the
# "host" kernel.  This has no impact on the host system.
# 
# Note also this script is idempotent - you can run it more than
# once, and you should in fact do so right now to update to a newer
# host kernel.
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

WORKDIR=`pwd`
cd `dirname $0`
SRCDIR=`pwd`
cd $WORKDIR

if test $(id -u) != 0; then
    cat <<EOF
This script should be run as root.
EOF
    exit 1
fi

mkdir -p /ostree

cd /ostree

mkdir -p modules
mkdir -p var

if ! test -d repo; then
    mkdir repo
    ostree --repo=repo init
fi

uname=$(uname -r)

if test -d /etc/grub.d; then
    cp $SRCDIR/15_ostree /etc/grub.d/
else
    cat <<EOF
GRUB 2 not detected; you'll need to edit e.g. /boot/grub/grub.conf manually
EOF
fi

kernel=/boot/vmlinuz-${uname}
if ! test -f "${kernel}"; then
    cat <<EOF
    Kernel does not exist: ${kernel}
EOF
    exit 1
fi

if ! test -d /ostree/modules/${uname}; then
    cp -ar /lib/modules/${uname} /ostree/modules/${uname}
fi

initrd_name=initramfs-ostree-${uname}.img
if ! test -f "/boot/${initrd_name}"; then
    initrd_tmpdir=$(mktemp -d '/tmp/gnomeos-dracut.XXXXXXXXXX')
    linux-user-chroot \
    --mount-readonly / \
    --mount-proc /proc \
    --mount-bind /dev /dev \
    --mount-bind /ostree/var /var \
    --mount-bind ${initrd_tmpdir} /tmp \
    --mount-bind /ostree/modules /lib/modules \
    /ostree/${BRANCH_PREFIX}devel \
    dracut -f /tmp/${initrd_name} "${uname}"
    mv "${initrd_tmpdir}/${initrd_name}" "/boot/${initrd_name}"
    rm -rf "${initrd_tmpdir}"
fi
