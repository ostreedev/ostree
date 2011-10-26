#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Generate a root filesystem image
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

OSTREE=${OSTREE:-ostree}

case `uname -p` in
    x86_64)
        ARCH=amd64
        ;;
    *)
        echo "Unsupported architecture"
        ;;
esac;

DEBTARGET=wheezy

INITRD_MOVE_MOUNTS="dev proc sys"
TOPROOT_BIND_MOUNTS="boot home root tmp"
OSTREE_BIND_MOUNTS="var"
MOVE_MOUNTS="selinux mnt media"
READONLY_BIND_MOUNTS="bin etc lib lib32 lib64 sbin usr"

OBJ=debootstrap-$DEBTARGET
if ! test -d ${OBJ} ; then
    echo "Creating $DEBTARGET.img"
    mkdir -p ${OBJ}.tmp
    debootstrap --download-only --arch $ARCH $DEBTARGET ${OBJ}.tmp
    mv ${OBJ}.tmp ${OBJ}
fi

OBJ=$DEBTARGET.img
if ! test -f ${OBJ}; then
    umount fs || true
    mkdir -p fs
    qemu-img create ${OBJ}.tmp 2G
    mkfs.ext4 -q -F ${OBJ}.tmp
    mount -o loop ${OBJ}.tmp fs

    for d in debootstrap-$DEBTARGET/var/cache/apt/archives/*.deb; do
        rm -rf work; mkdir work
        (cd work && ar x ../$d && tar -x -z -C ../fs -f data.tar.gz)
    done

    umount fs
    mv ${OBJ}.tmp ${OBJ}
fi

# TODO download source for above
# TODO download build dependencies for above

OBJ=gnomeos-filesystem.img
if ! test -f ${OBJ}; then
    cp -a --sparse=always $DEBTARGET.img ${OBJ}.tmp
    mkdir -p fs
    umount fs || true
    mount -o loop ${OBJ}.tmp fs
    (cd fs;
        mkdir ostree
        mkdir ostree/repo
        mkdir ostree/gnomeos-origin
        for d in $INITRD_MOVE_MOUNTS $TOPROOT_BIND_MOUNTS; do
            mkdir -p ostree/gnomeos-origin/$d
            chmod --reference $d ostree/gnomeos-origin/$d
        done
        for d in $OSTREE_BIND_MOUNTS; do
            mkdir -p ostree/gnomeos-origin/$d
            chmod --reference $d ostree/gnomeos-origin/$d
            mv $d ostree
        done
        for d in $READONLY_BIND_MOUNTS $MOVE_MOUNTS; do
            if test -d $d; then
                mv $d ostree/gnomeos-origin
            fi
        done

        cp ${SRCDIR}/debian-setup.sh ostree/gnomeos-origin/
        chroot ostree/gnomeos-origin ./debian-setup.sh
        rm ostree/gnomeos-origin/debian-setup.sh

        ostree init --repo=ostree/repo
        (cd ostree/gnomeos-origin; find . '!' -type p | grep -v '^.$' | $OSTREE commit -s 'Initial import' --repo=../repo --from-stdin)
        rm -rf ostree/gnomeos-origin
        (cd ostree;
            rev=`cat repo/HEAD`
            $OSTREE checkout --repo=repo HEAD gnomeos-${rev}
            $OSTREE run-triggers --repo=repo current
            ln -s gnomeos-${rev} current)
    )
    umount fs
    mv ${OBJ}.tmp ${OBJ}
fi

OBJ=gnomeos-kernel
if ! test -f ${OBJ}; then
    if test -x /sbin/grubby; then
        kernel=`grubby --default-kernel`
        cp $kernel ${OBJ}.tmp
    else
        echo "ERROR: couldn't find /sbin/grubby (which we use to find the running kernel)"
        echo "  You can copy any kernel image you want in here as gnomeos-kernel"
        echo "  For example: cp /boot/vmlinuz-2.6.40.6-0.fc15.x86_64 gnomeos-kernel"
        exit 1
    fi
    mv ${OBJ}.tmp ${OBJ}
fi

cp ${SRCDIR}/ostree_switch_root ${WORKDIR}

OBJ=gnomeos-initrd.img
if ! test -f ${OBJ}; then
    rm -f ${OBJ}.tmp
    dracutbasedir=/src/build/jhbuild/share/dracut /src/build/jhbuild/sbin/dracut -v --include `pwd`/ostree_switch_root /sbin/ostree_switch_root ${OBJ}.tmp
    mv ${OBJ}.tmp ${OBJ}
fi
