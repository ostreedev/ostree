#!/bin/sh
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

SRCDIR=`dirname $0`
WORKDIR=`pwd`

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

    $SRCDIR/ostree-setup.sh /ostree
fi

#ostree pull http://ostree.gnome.org/3.4/repo gnomeos-3.4-i686-{runtime,devel}
if ! test -f /ostree/repo/refs/heads/gnomeos-3.4-i686-runtime; then
    cat <<EOF
You must get a repo from somewhere...e.g.:
 cd /ostree && rsync --progress -ave ssh master.gnome.org:/home/users/walters/ostree/repo .
EOF
    exit 1
fi

uname=$(uname -r)

cd /ostree
for branch in runtime devel; do
    rev=$(ostree --repo=$(pwd)/repo rev-parse ${BRANCH_PREFIX}${branch});
    if ! test -d ${BRANCH_PREFIX}${branch}-${rev}; then
        ostree --repo=repo checkout ${rev} ${BRANCH_PREFIX}${branch}-${rev}
        ostbuild chroot-run-triggers ${BRANCH_PREFIX}${branch}-${rev}
        cp -ar /lib/modules/${uname} ${BRANCH_PREFIX}${branch}-${rev}/lib/modules/${uname}
    fi
    rm -f ${BRANCH_PREFIX}${branch}-current
    ln -s ${BRANCH_PREFIX}${branch}-${rev} ${BRANCH_PREFIX}${branch}-current
done
rm -f current
ln -s ${BRANCH_PREFIX}runtime-current current

cp -a ./${BRANCH_PREFIX}${branch}-current/usr/sbin/ostree-init .
cd -

if test -d /etc/grub.d; then
    cp $SRCDIR/15_ostree /etc/grub.d/
else
    cat <<EOF
GRUB 2 not detected; you'll need to edit e.g. /boot/grub/grub.conf manually
Kernel has been installed as /boot/bzImage-gnomeos.bin
EOF
fi

kernel=/boot/vmlinuz-${uname}
if ! test -f "${kernel}"; then
    cat <<EOF
    Kernel does not exist: ${kernel}
EOF
    exit 1
fi
initrd_name=initramfs-ostree-${uname}.img
initrd_tmpdir=$(mktemp -d '/tmp/gnomeos-dracut.XXXXXXXXXX')
linux-user-chroot \
    --mount-readonly / \
    --mount-proc /proc \
    --mount-bind /dev /dev \
    --mount-bind /ostree/var /var \
    --mount-bind ${initrd_tmpdir} /tmp \
    --mount-bind /lib/modules/${uname} /lib/modules/${uname} \
    /ostree/${BRANCH_PREFIX}devel-current \
    dracut -f /tmp/${initrd_name} "${uname}"
mv "${initrd_tmpdir}/${initrd_name}" "/boot/${initrd_name}"
rm -rf "${initrd_tmpdir}"
