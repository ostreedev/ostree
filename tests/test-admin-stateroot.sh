#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
# Copyright (C) 2022 Huijing Hei <hhei@redhat.com>
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..2"

# Initial deployment on testos stateroot
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime

REF=$(${CMD_PREFIX} ostree admin status | grep -oE 'testos .*\.0' | sed -e 's/^testos //' -e 's/\.0$//')

# Should prompt bootloader update as it does have a different stateroot
${CMD_PREFIX} ostree admin stateroot-init testos2
${CMD_PREFIX} ostree admin deploy ${REF} --os=testos2
${CMD_PREFIX} ostree admin set-default 1 >out.txt

assert_file_has_content out.txt 'bootconfig swap: yes'
echo "ok stateroot new-deployment"

# Should not prompt update to bootloader as stateroot/commit and kargs are all the same.
${CMD_PREFIX} ostree admin deploy ${REF} --os=testos2
${CMD_PREFIX} ostree admin deploy ${REF} --os=testos2
${CMD_PREFIX} ostree admin set-default 1 >out.txt

assert_file_has_content out.txt 'bootconfig swap: no'
echo "ok stateroot equal-deployment"
