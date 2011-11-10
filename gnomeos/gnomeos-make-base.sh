#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Generate a root filesystem image
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

DEPENDS="debootstrap"

for x in $DEPENDS; do
    if ! command -v $x; then
        cat <<EOF
Couldn't find required dependency $x
EOF
        exit 1
    fi
done

if test $(id -u) == 0; then
    echo "Should not run this script as root."
    exit 1
fi

if test -z "${OSTREE}"; then
    OSTREE=`command -v ostree || true`
fi
if test -z "${OSTREE}"; then
    cat <<EOF
ERROR:
Couldn't find ostree; you can set the OSTREE environment variable to point to it
e.g.: OSTREE=~user/checkout/ostree/ostree $0
EOF
    exit 1
fi

if test -z "$DRACUT"; then
    if ! test -d dracut; then
        cat <<EOF
Checking out and patching dracut...
EOF
        git clone git://git.kernel.org/pub/scm/boot/dracut/dracut.git
        (cd dracut; git am $SRCDIR/0001-Support-OSTree.patch)
        (cd dracut; make)
    fi
    DRACUT=`pwd`/dracut/dracut
fi

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

cd ${WORKDIR}
OBJ=debootstrap-$DEBTARGET
if ! test -d ${OBJ} ; then
    echo "Creating $DEBTARGET.img"
    mkdir -p ${OBJ}.tmp
    debootstrap --download-only --arch $ARCH $DEBTARGET ${OBJ}.tmp
    mv ${OBJ}.tmp ${OBJ}
fi

cd ${WORKDIR}
OBJ=$DEBTARGET-fs
if ! test -d ${OBJ}; then
    rm -rf ${OBJ}.tmp
    mkdir ${OBJ}.tmp

    cd ${OBJ}.tmp;
    mkdir -m 0755 $INITRD_MOVE_MOUNTS $TOPROOT_BIND_MOUNTS
    chmod a=rwxt tmp

    mkdir ostree

    mkdir -p -m 0755 ostree/var/{log,run}

    mkdir ostree/repo

    $OSTREE --repo=ostree/repo init 
    
    BRANCHES=""

    mkdir ostree/worktree
    cd ostree/worktree
    mkdir -m 0755 $INITRD_MOVE_MOUNTS $TOPROOT_BIND_MOUNTS $OSTREE_BIND_MOUNTS $READONLY_BIND_MOUNTS $MOVE_MOUNTS sysroot
    chmod a=rwxt tmp
    $OSTREE --repo=../repo commit -b gnomeos-filesystem -s "Base filesystem layout"
    BRANCHES="$BRANCHES gnomeos-filesystem"
    cd ..
    rm -rf worktree

    for d in ${WORKDIR}/debootstrap-$DEBTARGET/var/cache/apt/archives/*.deb; do
        bn=$(basename $d)
        debname=$(echo $bn | cut -f 1 -d _)
        debversion=$(echo $bn | cut -f 2 -d _)
        archivename="archive-${debname}"
        rm -rf worktree; mkdir worktree;
        cd worktree;
        mkdir data;
        ar x $d;
        tar -x -z -C data -f data.tar.gz;
        cd data;
        $OSTREE --repo=../../repo commit -b "${archivename}" -s "Version ${debversion}"
        BRANCHES="$BRANCHES $archivename"
        cd ../..
    done
    rm -rf worktree

    $OSTREE --repo=repo compose --out-metadata=./compose-meta worktree $BRANCHES
    cd worktree
    $OSTREE --repo=../repo commit --metadata-variant=../compose-meta -b gnomeos -s "Compose of Debian $DEBTARGET"
    cd ..
    rm -rf worktree
    
    cd ${WORKDIR}
    if test -d ${OBJ}; then
        mv ${OBJ} ${OBJ}.old
    fi
    mv ${OBJ}.tmp ${OBJ}
    rm -rf ${OBJ}.old
fi

# TODO download source for above
# TODO download build dependencies for above

cd ${WORKDIR}
OBJ=gnomeos-fs
if ! test -d ${OBJ}; then
    rm -rf ${OBJ}.tmp
    cp -al $DEBTARGET-fs ${OBJ}.tmp
    cd ${OBJ}.tmp/ostree;
    rm -rf worktree
    $OSTREE --repo=repo checkout gnomeos worktree
    cd worktree
    ${SRCDIR}/debian-setup.sh
    $OSTREE --repo=../repo commit -b gnomeos -s "Run debian-setup.sh"
    cd ..
    rm -rf worktree

    rev=$($OSTREE --repo=repo rev-parse gnomeos);
    $OSTREE --repo=repo checkout ${rev} gnomeos-${rev}
    $OSTREE --repo=repo run-triggers gnomeos-${rev}
    ln -s gnomeos-${rev} current
    
    cd ${WORKDIR}
    if test -d ${OBJ}; then
        mv ${OBJ} ${OBJ}.old
    fi
    mv ${OBJ}.tmp ${OBJ}
    rm -rf ${OBJ}.old
fi

cd ${WORKDIR}
cp ${SRCDIR}/ostree_switch_root ${WORKDIR}

kv=`uname -r`
kernel=/boot/vmlinuz-${kv}
if ! test -f "${kernel}"; then
    cat <<EOF
Failed to find ${kernel}
EOF
fi

cd ${WORKDIR}
OBJ=gnomeos-initrd.img
VOBJ=gnomeos-initrd-${kv}.img
if ! test -f ${OBJ}; then
    rm -f ${OBJ}.tmp ${VOBJ}.tmp
    $DRACUT -l -v -o plymouth --include `pwd`/ostree_switch_root /sbin/ostree_switch_root ${VOBJ}.tmp
    mv ${VOBJ}.tmp ${VOBJ}
    ln -sf ${VOBJ} gnomeos-initrd.img
fi
