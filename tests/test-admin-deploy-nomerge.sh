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

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime

echo "1..1"
${CMD_PREFIX} ostree admin deploy --os=testos --karg=root=LABEL=foo --karg=testkarg=1 testos:testos/buildmaster/x86_64-runtime
origdeployment=$(${CMD_PREFIX} ostree admin --sysroot=sysroot --print-current-dir)
testconfig=etc/modified-config-file-that-will-be-removed
touch "${origdeployment}"/"${testconfig}"
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf "^options.*root=LABEL=foo.*testkarg"
${CMD_PREFIX} ostree admin deploy --os=testos --no-merge --karg=root=LABEL=bar testos:testos/buildmaster/x86_64-runtime
deployment=$(${CMD_PREFIX} ostree admin --sysroot=sysroot --print-current-dir)
assert_not_streq "${origdeployment}" "${deployment}"
assert_not_has_file "${deployment}/${testconfig}"
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf "^options root=LABEL=bar"
assert_not_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf "^options .*testkarg"
echo "ok no merge deployment"
