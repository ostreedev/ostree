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
# So cleanup works on failure too
trap 'umount sysroot/boot; _tmpdir_cleanup' EXIT
# A VFAT /boot is supported, with boot/loader as a directory
ostree admin status --sysroot=sysroot >out.txt
assert_file_has_content_literal out.txt "No deployments."

# Deploy, then update the kernel. Keep the deployments mutable for cleanup.
export OSTREE_SYSROOT_DEBUG=mutable-deployments
mkdir -p osdata/usr/lib/modules/6.1.0 osdata/usr/etc
echo "a kernel" > osdata/usr/lib/modules/6.1.0/vmlinuz
echo "an initramfs" > osdata/usr/lib/modules/6.1.0/initramfs.img
printf 'ID=testos\nPRETTY_NAME=TestOS\n' > osdata/usr/etc/os-release
ostree --repo=sysroot/ostree/repo commit -b testos --tree=dir=osdata
ostree --repo=sysroot/ostree/repo config set sysroot.bootloader none
ostree admin stateroot-init --sysroot=sysroot testos
ostree admin deploy --sysroot=sysroot --os=testos testos --karg=root=LABEL=x
if test -L sysroot/boot/loader || ! test -d sysroot/boot/loader; then
    fatal "boot/loader is not a directory"
fi
echo "timeout 3" > sysroot/boot/loader/loader.conf
echo "a new kernel" > osdata/usr/lib/modules/6.1.0/vmlinuz
ostree --repo=sysroot/ostree/repo commit -b testos --tree=dir=osdata
ostree admin deploy --sysroot=sysroot --os=testos testos >out.txt
assert_file_has_content_literal out.txt "bootconfig swap: yes"
if test -e sysroot/boot/loader.tmp || test -e sysroot/boot/loader.old; then
    fatal "leftover boot/loader.tmp or boot/loader.old"
fi
assert_file_has_content_literal sysroot/boot/loader/loader.conf "timeout 3"
assert_file_has_content_literal sysroot/boot/loader/entries/ostree-1.conf "root=LABEL=x"
assert_file_has_content_literal sysroot/boot/loader/entries/ostree-2.conf "root=LABEL=x"
assert_streq "$(cat sysroot/boot/ostree/testos-*/vmlinuz-6.1.0 | sort | tr '\n' ' ')" "a kernel a new kernel "
