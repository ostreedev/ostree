#!/bin/bash
# -*- indent-tabs-mode: nil; -*-
# Install OSTree to system
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

usage () {
    echo "$0 OSTREE_REPO_URL"
    exit 1
}

ARCH=i686
BRANCH_PREFIX="gnomeos-3.4-${ARCH}-"

if ! test -d /ostree/repo/objects; then
    mkdir -p /ostree

    $SRCDIR/gnomeos-setup.sh /ostree
fi

cd /ostree

ostree --repo=repo remote add gnome http://ostree.gnome.org/repo ${BRANCH_PREFIX}{runtime,devel}
ostree-pull --repo=repo gnome
for branch in runtime devel; do
    ostree --repo=repo checkout --atomic-retarget ${BRANCH_PREFIX}${branch}
done
ln -sf ${BRANCH_PREFIX}runtime current

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
