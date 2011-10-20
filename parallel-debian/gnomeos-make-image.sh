#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Generate an ext2 root filesystem disk image.
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

DEBTARGET=squeeze
KERNEL_BIN=http://ftp.us.debian.org/debian/pool/main/l/linux-latest-2.6/linux-image-2.6-amd64_2.6.32+29_amd64.deb

NOTSHARED_DIRS="dev bin etc lib lib32 lib64 proc media mnt run sbin selinux sys srv usr"
SHARED_DIRS="home root opt tmp var"

case `uname -p` in
    x86_64)
        ARCH=amd64
        ;;
    i?86)
        ARCH=i386
        ;;
esac;

if ! test -f $DEBTARGET.img; then
    echo "Creating $DEBTARGET.img"
    qemu-img create $DEBTARGET.img.tmp 2G
    mkfs.ext2 -q -F $DEBTARGET.img.tmp
    mkdir -p gnomeos-fs
    mount -o loop $DEBTARGET.img.tmp gnomeos-fs
    debootstrap --arch $ARCH $DEBTARGET gnomeos-fs
    umount gnomeos-fs
    mv $DEBTARGET.img.tmp $DEBTARGET.img
fi

if ! test -f 

if ! test -f gnomeos.img; then
    echo "Cloning gnomeos.img from $DEBTARGET.img"
    cp -a --sparse=always $DEBTARGET.img gnomeos.img.tmp
    umount gnomeos-fs || true
    mount -o loop gnomeos.img.tmp gnomeos-fs
    (cd gnomeos-fs;
        mkdir ostree
        mkdir ostree/repo
        mkdir ostree/gnomeos-origin
        for d in $NOTSHARED_DIRS; do
            if test -d $d; then
                mv $d ostree/gnomeos-origin
            fi
        done
	for d in $SHARED_DIRS; do
	    mv $d ostree
	    mkdir ostree/gnomeos-origin/$d
	    touch ostree/gnomeos-origin/$d/EMPTY
        done
        ostree init --repo=ostree/repo
        (cd ostree/gnomeos-origin; find . '!' -type p | grep -v '^.$' | ostree commit -s 'Initial import' --repo=../repo --from-stdin)
        rm -rf ostree/gnomeos-origin
        (cd ostree;
            rev=`cat repo/HEAD`
            ostree checkout --repo=repo HEAD gnomeos-${rev}
            ln -s gnomeos-${rev} current)
    )
    umount gnomeos-fs
    mv gnomeos.img.tmp gnomeos.img
fi


