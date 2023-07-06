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
rm test2-co/whiteouts -rf  # This may or may not exist
COMMIT_ARGS="--owner-uid=0 --owner-gid=0 --no-xattrs --canonical-permissions"
$OSTREE commit ${COMMIT_ARGS} -b test-composefs --generate-composefs-metadata test2-co
# If the test fails we'll dump this out
$OSTREE ls -RCX test-composefs /
orig_composefs_digest=$($OSTREE show --print-hex --print-metadata-key ostree.composefs.digest.v0 test-composefs)
$OSTREE commit ${COMMIT_ARGS} -b test-composefs2 --generate-composefs-metadata test2-co
new_composefs_digest=$($OSTREE show --print-hex --print-metadata-key ostree.composefs.digest.v0 test-composefs2)
assert_streq "${orig_composefs_digest}" "${new_composefs_digest}"
assert_streq "${new_composefs_digest}" "4d97f295ac9d379b7b3c42ddec49cc55b570c5b8b2a6f3f835f473854b0ad0cd"
tap_ok "composefs metadata"

tap_end
