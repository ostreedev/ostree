#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+
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

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

extra_admin_tests=3

. $(dirname $0)/admin-test.sh

# Test the legacy dirs
for test_bootdir in "boot" "usr/lib/ostree-boot"; do
    cd ${test_tmpdir}
    rm httpd osdata testos-repo sysroot -rf
    setup_os_repository "archive" "syslinux" $test_bootdir
    ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
    rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
    ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
    assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
    assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* quiet'
    assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
    assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/initramfs-3.6.0.img 'an initramfs'
    # kernel/initrams should also be in the tree's /boot with the checksum
    assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/$test_bootdir/vmlinuz-3.6.0-${bootcsum} 'a kernel'
    assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/$test_bootdir/initramfs-3.6.0.img-${bootcsum} 'an initramfs'
    assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
    assert_file_has_content sysroot/ostree/boot.1/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
    ${CMD_PREFIX} ostree admin status
    validate_bootloader
    echo "ok kernel in $test_bootdir"
done

# And test that legacy overrides /usr/lib/modules
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "syslinux" "usr/lib/ostree-boot"
cd osdata
echo "this is a kernel without an initramfs like Fedora 26" > usr/lib/modules/3.6.0/vmlinuz
usrlib_modules_bootcsum=$(cat usr/lib/modules/3.6.0/vmlinuz | sha256sum | cut -f 1 -d ' ')
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmaster/x86_64-runtime -s "Build"
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/initramfs-3.6.0.img 'an initramfs'
# Note this bootcsum shouldn't be the modules one
assert_not_streq "${bootcsum}" "${usrlib_modules_bootcsum}"
echo "ok kernel in /usr/lib/modules and /usr/lib/ostree-boot"
