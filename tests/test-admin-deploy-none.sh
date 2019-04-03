#!/bin/bash
#
# Copyright (C) 2019 Robert Fairley <rfairley@redhat.com>
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
setup_os_repository "archive" "sysroot.bootloader none"

extra_admin_tests=1

. $(dirname $0)/admin-test.sh

# Test that the bootloader configuration "none" generates BLS config snippets.
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "sysroot.bootloader none"
${CMD_PREFIX} ostree pull-local --repo=sysroot/ostree/repo --remote testos testos-repo testos/buildmaster/x86_64-runtime
# Test that configuring sysroot.bootloader="none" is a workaround for previous
# grub2 bootloader issue (see https://github.com/ostreedev/ostree/issues/1774)
mkdir -p sysroot/boot/grub2
touch sysroot/boot/grub2/grub.cfg
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os testos testos/buildmaster/x86_64-runtime > out.txt
assert_file_has_content out.txt "Bootloader updated.*"
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/initramfs-3.6.0.img 'an initramfs'
echo "ok generate bls config on first deployment"

# TODO: add tests to try setting with an unsupported bootloader config,
# once https://github.com/ostreedev/ostree/issues/1827 is solved.
# The tests should check that the following commands fail:
#  ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootloader unsupported_bootloader
#  ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootloader ""
