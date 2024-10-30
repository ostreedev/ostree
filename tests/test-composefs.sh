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

skip_without_ostree_feature composefs
skip_without_user_xattrs

setup_test_repository "bare-user"

cd ${test_tmpdir}
$OSTREE checkout test2 test2-co
rm test2-co/whiteouts -rf  # This may or may not exist
COMMIT_ARGS="--owner-uid=0 --owner-gid=0 --no-xattrs --canonical-permissions"
$OSTREE commit ${COMMIT_ARGS} -b test-composefs-without-meta test2-co
$OSTREE commit ${COMMIT_ARGS} -b test-composefs --generate-composefs-metadata test2-co
# If the test fails we'll dump this out
$OSTREE ls -RCX test-composefs /
orig_composefs_digest=$($OSTREE show --print-hex --print-metadata-key ostree.composefs.digest.v0 test-composefs)
$OSTREE commit ${COMMIT_ARGS} -b test-composefs2 --generate-composefs-metadata test2-co
new_composefs_digest=$($OSTREE show --print-hex --print-metadata-key ostree.composefs.digest.v0 test-composefs2)
assert_streq "${orig_composefs_digest}" "${new_composefs_digest}"
assert_streq "${new_composefs_digest}" "be956966c70970ea23b1a8043bca58cfb0d011d490a35a7817b36d04c0210954"
rm test2-co -rf
tap_ok "composefs metadata"

$OSTREE checkout --composefs test-composefs test2-co.cfs
digest=$(sha256sum < test2-co.cfs | cut -f 1 -d ' ')
# This file should be reproducible bit for bit across environments; per above
# we're operating on predictable data (fixed uid, gid, timestamps, xattrs, permissions).
assert_streq "${digest}" "031fab2c7f390b752a820146dc89f6880e5739cba7490f64024e0c7d11aad7c9"
# Verify it with composefs tooling
composefs-info dump test2-co.cfs > dump.txt
# Verify we have a verity digest
assert_file_has_content_literal dump.txt '/baz/cow 4 100644 1 0 0 0 0.0 f6/a517d53831a40cff3886a965c70d57aa50797a8e5ea965b2c49cc575a6ff51.file - ebaa23af194a798df610e5fe2bd10725c9c4a3a56a6b62d4d0ee551d4fc4be27'
rm -vf dump.txt test2-co.cfs
tap_ok "checkout composefs"

$OSTREE checkout --composefs-noverity test-composefs-without-meta test2-co-noverity.cfs
digest=$(sha256sum < test2-co-noverity.cfs | cut -f 1 -d ' ')
# Should be reproducible per above
assert_streq "${digest}" "78f873a76ccfea3ad7c86312ba0e06f8e0bca54ab4912b23871b31caafe59c24"
# Verify it with composefs tooling
composefs-info dump test2-co-noverity.cfs > dump.txt
# No verity digest here
assert_file_has_content_literal dump.txt '/baz/cow 4 100644 1 0 0 0 0.0 f6/a517d53831a40cff3886a965c70d57aa50797a8e5ea965b2c49cc575a6ff51.file - -'
tap_ok "checkout composefs noverity"

# Test with a corrupted composefs digest
$OSTREE commit ${COMMIT_ARGS} -b test-composefs-bad-digest --tree=ref=test-composefs \
    '--add-metadata=ostree.composefs.digest.v0=[byte 0x13, 0xae, 0xae, 0xed, 0xc0, 0x34, 0xd1, 0x39, 0xef, 0xfc, 0xd6, 0x6f, 0xe3, 0xdb, 0x08, 0xd3, 0x32, 0x8a, 0xec, 0x2f, 0x02, 0xc5
, 0xa7, 0x8a, 0xee, 0xa6, 0x0f, 0x34, 0x6d, 0x7a, 0x22, 0x6d]'
if $OSTREE checkout --composefs test-composefs-bad-digest test2-co.cfs 2>err.txt; then
    fatal "checked out composefs with mismatched digest"
fi
assert_file_has_content_literal err.txt "doesn't match expected digest"
tap_ok "checkout composefs bad digest"

tap_end
