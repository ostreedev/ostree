#!/bin/sh

set -e
set -x

DIRS="bin boot dev etc lib lib64 media mnt opt proc root run sbin selinux srv sys tmp usr var"

if ! test -f debian.img; then
  echo need debian.img
  exit 1
fi

mount -o loop debian.img debian-mnt
cd debian-mnt
if ! test -d r0; then
  mkdir r0
  mv $DIRS r0
fi
cd ..
umount debian-mnt
