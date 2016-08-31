#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..2"

setup_os_repository "archive-z2" "uboot"

cd ${test_tmpdir}

ln -s ../../boot/ osdata/usr/lib/ostree-boot
echo "1" > osdata/boot/1
mkdir -p osdata/boot/subdir
ln -s ../1 osdata/boot/subdir/2

${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --os=testos testos:testos/buildmaster/x86_64-runtime

assert_has_file sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0
assert_not_has_file sysroot/boot/ostree/testos-${bootcsum}/1

echo "ok boot dir without .ostree-bootcsumdir-source"

touch osdata/boot/.ostree-bootcsumdir-source
${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos

assert_has_file sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0
assert_has_file sysroot/boot/ostree/testos-${bootcsum}/1
assert_has_file sysroot/boot/ostree/testos-${bootcsum}/subdir/2
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/subdir/2 "1"

echo "ok boot dir with .ostree-bootcsumdir-source"
