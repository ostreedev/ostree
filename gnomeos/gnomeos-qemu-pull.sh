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
    cat <<EOF 
usage: $0 SRC_REPO_PATH CURRENT_REF [REFS...]
EOF
    exit 1
}

SRC_REPO_PATH=$1
test -n "$SRC_REPO_PATH" || usage
shift

CURRENT_REF=$1
test -n "$CURRENT_REF" || usage
shift

if ! test -f ostree-qemu.img; then
    cat <<EOF
ostree-qemu.img not found; You must run gnomeos-qemu-create.sh first
EOF
fi

mkdir -p fs
umount fs || true
sleep 1 # Avoid Linux kernel bug, pretty sure it's the new RCU pathname lookup
mount -o loop ostree-qemu.img fs

cd fs
ostree --repo=./ostree/repo pull-local ${SRC_REPO_PATH} ${CURRENT_REF} "$@"

cd ostree
ostree --repo=./repo checkout --atomic-retarget ${CURRENT_REF}
ln -sf ${CURRENT_REF} ${CURRENT_REF}.tmplink
mv -T ${CURRENT_REF}.tmplink current

cd ${WORKDIR}
umount fs
