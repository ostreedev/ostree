#!/bin/bash
# -*- indent-tabs-mode: nil; -*-
# Create ostree-qemu.img file in the current directory, suitable
# for booting via qemu.
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

OBJ=ostree-qemu.img
if ! test -f ${OBJ}; then
    # Hardcoded 6 gigabyte filesystem size here; 6 gigabytes should be
    # enough for everybody.
    qemu-img create $OBJ 6G
    mkfs.ext4 -q -F $OBJ
fi

mkdir -p fs
umount fs || true
sleep 1 # Avoid Linux kernel bug, pretty sure it's the new RCU pathname lookup
mount -o loop ostree-qemu.img fs

cd fs

if ! test -d ./ostree/repo/objects; then
    mkdir -p ./ostree
    
    $SRCDIR/gnomeos-setup.sh $(pwd)/ostree
fi

mkdir -p ./run ./home ./root ./sys
mkdir -p ./tmp 
chmod 01777 ./tmp

mkdir -p $(pwd)/ostree/modules
rsync -a -H -v --delete /ostree/modules/ ./ostree/modules/

cd ..
umount fs

cat << EOF
Next, run gnomeos-qemu-pull.sh to copy data.
EOF
