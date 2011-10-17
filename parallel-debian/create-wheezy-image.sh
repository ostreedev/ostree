#!/bin/sh

set -e
set -x

if test -f debian.img; then
  echo debian.img already exists
  exit 1
fi

qemu-img create debian.img 600M
mkfs.ext2 -q -F debian.img
mkdir -p debian-mnt
mount -o loop debian.img debian-mnt
debootstrap --arch amd64 wheezy debian-mnt
umount debian-mnt
