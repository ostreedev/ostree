#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
# Copyright (C) 2013 Javier Martinez <javier.martinez@collabora.co.uk>
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
setup_os_repository "archive" "uboot"

extra_admin_tests=1

. $(dirname $0)/admin-test.sh

cd ${test_tmpdir}
# Note this test actually requires a checksum change to /boot,
# because adding the uEnv.txt isn't currently covered by the
# "bootcsum".
os_repository_new_commit "uboot test" "test upgrade multiple kernel args"
mkdir -p osdata/usr/lib/ostree-boot
cat << 'EOF' > osdata/usr/lib/ostree-boot/uEnv.txt
loaduimage=load mmc ${bootpart} ${loadaddr} ${kernel_image}
loadfdt=load mmc ${bootpart} ${fdtaddr} ${bootdir}${fdtfile}
loadramdisk=load mmc ${bootpart} ${rdaddr} ${ramdisk_image}
mmcargs=setenv bootargs $bootargs console=${console} ${optargs} root=${mmcroot} rootfstype=${mmcrootfstype}
mmcboot=run loadramdisk; echo Booting from mmc ....; run mmcargs; bootz ${loadaddr} ${rdaddr} ${fdtaddr}
EOF
${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_file_has_content sysroot/boot/uEnv.txt "loadfdt="
assert_file_has_content sysroot/boot/uEnv.txt "kernel_image="
assert_file_has_content sysroot/boot/uEnv.txt "kernel_image2="
assert_file_has_content sysroot/boot/uEnv.txt "kernel_image3="

echo "ok merging uEnv.txt files"
