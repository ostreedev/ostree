#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
# Copyright (C) 2015 Red Hat, Inc.
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

if ! has_gpgme; then
    echo "1..0 #SKIP no gpgme support compiled in"
    exit 0
fi

echo "1..1"

setup_test_repository "archive"

export OSTREE_GPG_SIGN="${OSTREE} gpg-sign --gpg-homedir=${TEST_GPG_KEYHOME}"

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
# We at least got some content here and ran through the code; later
# tests will actually do verification
assert_file_has_content test2-show 'Found 1 signature'

${OSTREE} show --gpg-homedir=${TEST_GPG_KEYHOME} test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 1 signature'

# Now sign a commit with 3 different keys
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID_1} --gpg-sign=${TEST_GPG_KEYID_2} --gpg-sign=${TEST_GPG_KEYID_3} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 3 signature'

# Commit and sign separately, then monkey around with signatures
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --tree=dir=files
if ${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature'; then
  assert_not_reached
fi
${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_1}
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 1 signature'
# Signing with a previously used key should be caught
if ${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_1} 2>/dev/null; then
  assert_not_reached
fi
# Add a few more signatures and then delete them
${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3}
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 3 signature'
${OSTREE_GPG_SIGN} --delete test2 ${TEST_GPG_KEYID_2} | grep -o 'Signatures deleted: [[:digit:]]' > test2-delete
assert_file_has_content test2-delete 'Signatures deleted: 1'
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 2 signature'
# Already deleted TEST_GPG_KEYID_2; should be ignored
${OSTREE_GPG_SIGN} --delete test2 ${TEST_GPG_KEYID_1} ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3} | grep -o 'Signatures deleted: [[:digit:]]' > test2-delete
assert_file_has_content test2-delete 'Signatures deleted: 2'
# Verify all signatures are gone
if ${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature'; then
  assert_not_reached
fi

libtest_cleanup_gpg

echo "ok"
