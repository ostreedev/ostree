#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Run built image in QEMU 
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

set -e
set -x

SRCDIR=`dirname $0`
WORKDIR=`pwd`

if ! test -d gnomeos-fs; then
    echo "Error: couldn't find gnomeos-fs. Run gnomeos-make-image.sh"
    exit 1
fi

OBJ=gnomeos-fs.img
if (! test -f ${OBJ}) || test gnomeos-fs -nt ${OBJ}; then
    rm -f ${OBJ}.tmp
    qemu-img create ${OBJ}.tmp 2G
    mkfs.ext4 -q -F ${OBJ}.tmp
    mkdir -p fs
    umount fs || true
    mount -o loop ${OBJ}.tmp fs
    cp -a gnomeos-fs/* fs
    umount fs
    mv ${OBJ}.tmp ${OBJ}
fi

exec qemu-kvm -kernel `grubby --default-kernel` -initrd gnomeos-initrd.img -hda gnomeos-fs.img -append "root=/dev/sda ostree=current"
