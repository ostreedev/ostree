#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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

echo "1..20"

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive-z2" "syslinux"

. $(dirname $0)/admin-test.sh

cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive-z2" "syslinux" "boot"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* quiet'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/initramfs-3.6.0 'an initramfs'
# kernel/initrams should also be in the tree's /boot with the checksum
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/boot/vmlinuz-3.6.0-${bootcsum} 'a kernel'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/boot/initramfs-3.6.0-${bootcsum} 'an initramfs'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.1/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader
echo "ok kernel in tree's /boot"
