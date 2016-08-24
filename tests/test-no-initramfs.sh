#!/bin/bash

. $(dirname $0)/libtest.sh

echo "1..3"

setup_os_repository "archive-z2" "uboot"

cd ${test_tmpdir}

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=rootfs --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'root=LABEL=rootfs'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'init='

echo "ok deployment with initramfs"

cd ${test_tmpdir}/osdata/boot
rm -f initramfs* vmlinuz*
echo "the kernel only" > vmlinuz-3.6.0
bootcsum=$(cat vmlinuz-3.6.0 | sha256sum | cut -f 1 -d ' ')
mv vmlinuz-3.6.0 vmlinuz-3.6.0-${bootcsum}
cd -
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree pull testos:testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos --karg=root=/dev/sda2 --karg=rootwait testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'rootwait'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'init='
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'initrd'

echo "ok switching to bootdir with no initramfs"

cd ${test_tmpdir}/osdata/boot
rm -f initramfs* vmlinuz*
echo "the kernel" > vmlinuz-3.6.0
echo "initramfs to assist the kernel" > initramfs-3.6.0
bootcsum=$(cat vmlinuz-3.6.0 initramfs-3.6.0 | sha256sum | cut -f 1 -d ' ')
mv vmlinuz-3.6.0 vmlinuz-3.6.0-${bootcsum}
mv initramfs-3.6.0 initramfs-3.6.0-${bootcsum}
cd -
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree pull testos:testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos --karg-none --karg=root=LABEL=rootfs testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'initrd'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'root=LABEL=rootfs'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'rootwait'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'init='

echo "ok switching from no initramfs to initramfs enabled sysroot"

