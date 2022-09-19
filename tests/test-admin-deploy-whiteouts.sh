#!/bin/bash
#
# Copyright (C) 2022 Red Hat, Inc.
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

set -euox pipefail

. $(dirname $0)/libtest.sh

skip_without_whiteouts_devices

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime

echo "1..3"
${CMD_PREFIX} ostree admin deploy --os=testos --karg=root=LABEL=foo --karg=testkarg=1 testos:testos/buildmain/x86_64-runtime
origdeployment=$(${CMD_PREFIX} ostree admin --sysroot=sysroot --print-current-dir)

assert_is_whiteout_device "${origdeployment}"/usr/container/layers/abcd/whiteout
echo "ok whiteout deployment"

assert_not_has_file  "${origdeployment}"/usr/container/layers/abcd/.ostree-wh.whiteout
echo "ok .ostree-wh.whiteout not created"

assert_file_has_mode "${origdeployment}"/usr/container/layers/abcd/whiteout 400
assert_file_has_mode "${origdeployment}"/usr/container/layers/abcd/whiteout2 777
echo "ok whiteout permissions are preserved"
