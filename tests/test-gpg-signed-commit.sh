#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
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

set -e

if ! ostree --version | grep -q -e '\+gpgme'; then
    exit 77
fi

. $(dirname $0)/libtest.sh

setup_test_repository "archive-z2"

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
$OSTREE show --print-detached-metadata-key=ostree.gpgsigs test2 > test2-gpgsigs
# We at least got some content here and ran through the code; later
# tests will actually do verification
assert_file_has_content test2-gpgsigs 'byte '

# Now sign a commit 3 times (with the same key)
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID} --gpg-sign=${TEST_GPG_KEYID} --gpg-sign=${TEST_GPG_KEYID} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
$OSTREE show --print-detached-metadata-key=ostree.gpgsigs test2 > test2-gpgsigs
assert_file_has_content test2-gpgsigs 'byte '

# Commit and sign separately
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --tree=dir=files
$OSTREE show --print-detached-metadata-key=ostree.gpgsigs test2 2> /dev/null && (echo 1>&2 "unsigned commit unexpectedly had detached metadata"; exit 1)
$OSTREE gpg-sign test2 ${TEST_GPG_KEYID} --gpg-homedir=${TEST_GPG_KEYHOME}
$OSTREE show --print-detached-metadata-key=ostree.gpgsigs test2 > test2-gpgsigs
assert_file_has_content test2-gpgsigs 'byte '
