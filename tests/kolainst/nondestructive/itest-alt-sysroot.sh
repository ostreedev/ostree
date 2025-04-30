#!/bin/bash

# Run test-basic.sh as root.
# https://github.com/ostreedev/ostree/pull/1199

set -xeuo pipefail

if test $(readlink /proc/1/ns/mnt) = $(readlink /proc/self/ns/mnt); then
    exec unshare -m -- "$0" "$@"
fi

. ${KOLA_EXT_DATA}/libinsttest.sh

# Use /var/tmp to hopefully use XFS + O_TMPFILE etc.
prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT

truncate -s 512M boot.img
mkfs.vfat boot.img

mkdir sysroot
ostree admin init-fs -E 1 sysroot
mount -o loop boot.img sysroot/boot
if ostree admin status --sysroot=sysroot 2>err.txt; then
    # So cleanup works on failure too
    umount sysroot/boot
    fatal "computed status on vfat sysroot"
fi
umount sysroot/boot
assert_file_has_content_literal err.txt "/boot cannot currently be a vfat filesystem"
