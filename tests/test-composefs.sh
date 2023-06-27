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

if ! ${CMD_PREFIX} ostree --version | grep -q -e '- composefs'; then
    echo "1..0 #SKIP no composefs support compiled in"
    exit 0
fi


setup_test_repository "bare-user"

cd ${test_tmpdir}
$OSTREE checkout test2 test2-co
$OSTREE commit ${COMMIT_ARGS} -b test-composefs --generate-composefs-metadata test2-co
orig_composefs_digest=$($OSTREE show --print-metadata-key ostree.composefs.v0 test-composefs)
assert_streq "${orig_composefs_digest}" '[byte 0x1f, 0x08, 0xe5, 0x8b, 0x14, 0x3b, 0x75, 0x34, 0x76, 0xb5, 0xef, 0x0c, 0x0c, 0x6e, 0xce, 0xbf, 0xde, 0xbb, 0x6d, 0x40, 0x30, 0x5e, 0x35, 0xbd, 0x6f, 0x8e, 0xc1, 0x9c, 0xd0, 0xd1, 0x5b, 0xae]'
$OSTREE commit ${COMMIT_ARGS} -b test-composefs2 --generate-composefs-metadata test2-co
new_composefs_digest=$($OSTREE show --print-metadata-key ostree.composefs.v0 test-composefs)
assert_streq "${orig_composefs_digest}" "${new_composefs_digest}"
tap_ok "composefs metadata"

tap_end
