#!/bin/bash
#
# Copyright (C) 2020 Red Hat, Inc.
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

echo "1..1"

# Exports OSTREE_SYSROOT so --sysroot not needed.
kver="3.6.0"
modulesdir="usr/lib/modules/${kver}"
setup_os_repository "archive" "syslinux" ${modulesdir}

cd ${test_tmpdir}
os_repository_new_commit "test" "test with device tree directory"

devicetree_path=osdata/${modulesdir}/dtb/asoc-board.dtb
devicetree_overlay_path=osdata/${modulesdir}/dtb/overlays/overlay.dtbo

mkdir -p osdata/${modulesdir}/dtb
echo "a device tree" > ${devicetree_path}
mkdir -p osdata/${modulesdir}/dtb/overlays
echo "a device tree overlay" > ${devicetree_overlay_path}

${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} env OSTREE_SYSROOT_DEBUG=${OSTREE_SYSROOT_DEBUG},no-dtb ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_file sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0
assert_not_has_file sysroot/boot/ostree/testos-${bootcsum}/dtb/asoc-board.dtb 'a device tree'
assert_streq $(ls sysroot/boot/ostree | wc -l) 1
assert_streq $(find sysroot/boot/ostree -name '*.dtb' | wc -l) 0
${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
env OSTREE_SYSROOT_DEBUG=${OSTREE_SYSROOT_DEBUG},no-dtb ${CMD_PREFIX} ostree admin upgrade --os=testos
${CMD_PREFIX} ostree --repo=testos-repo commit --tree=dir=osdata/ -b testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_streq $(ls sysroot/boot/ostree | wc -l) 2
# Note that the bootcsum computed by the test suite doesn't include devicetree
# And currently we end up installing the dtb for the *previous* deployment
# too which is a bug - in the future this should be fixed to assert 1.
assert_streq $(find sysroot/boot/ostree -name '*.dtb' | wc -l) 2

echo "ok update with no dtb to dtb"
