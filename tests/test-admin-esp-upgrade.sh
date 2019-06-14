#!/bin/bash
#
# Copyright (C) 2019 Red Hat, Inc
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

id=$(id -u)

if test ${id} != 0; then
    skip "this test needs to set up mount namespaces, rerun as root"
fi

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "sysroot.bootloader none"

echo "1..1"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

usr=sysroot/ostree/deploy/testos/deploy/${rev}.0/usr

# Create /usr/lib/ostree-boot/efi dir and some test files
mkdir -p ${usr}/lib/ostree-boot/efi/EFI
touch ${usr}/lib/ostree-boot/efi/file-a
touch ${usr}/lib/ostree-boot/efi/EFI/file-b

cd ${test_tmpdir}

# ostree-admin-esp-upgrade checks if /sys/firmware/efi exists
# and /boot/efi is a mountpoint.
mkdir -p sysroot/sys/firmware/efi
mkdir -p sysroot/boot/efi
mount --bind sysroot/boot/efi /boot/efi

${CMD_PREFIX} ostree admin esp-upgrade

assert_has_file sysroot/boot/efi/file-a
assert_has_file sysroot/boot/efi/EFI/file-b

umount /boot/efi

echo "ok"
