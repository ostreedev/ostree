#!/bin/bash
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

setup_os_repository "archive" "syslinux"

echo "1..1"
cd ${test_tmpdir}/osdata
mkdir etc
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=42.etc" -b testos/buildmain/x86_64-runtime
cd -
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
origdeployment=$(${CMD_PREFIX} ostree admin --sysroot=sysroot --print-current-dir)
assert_file_has_content ${origdeployment}/etc/NetworkManager/nm.conf "a default daemon file"
echo "ok empty etc"
