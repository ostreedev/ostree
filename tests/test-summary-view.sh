#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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
#
# Authors:
#  - Philip Withnall <withnall@endlessm.com>

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..2"

COMMIT_SIGN=""
SUMMARY_HOMEDIR=""
if has_ostree_feature gpgme; then
    COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"
    SUMMARY_HOMEDIR="--gpg-homedir=${TEST_GPG_KEYHOME}"
fi

setup_fake_remote_repo1 "archive" "${COMMIT_SIGN}"

# Set up a second branch.
mkdir ${test_tmpdir}/ostree-srv/other-files
cd ${test_tmpdir}/ostree-srv/other-files
echo 'hello world some object' > hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b other -s "A commit" -m "Example commit body"

# Generate the summary file.
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

# Check out the repository.
prev_dir=`pwd`
cd ${test_tmpdir}
ostree_repo_init repo --mode=archive
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --mirror origin

# Check the summary file exists in the checkout, and can be viewed.
assert_has_file repo/summary
${OSTREE} summary ${SUMMARY_HOMEDIR} --view > summary.txt
assert_file_has_content_literal summary.txt "* main"
assert_file_has_content_literal summary.txt "* other"
assert_file_has_content_literal summary.txt "ostree.summary.last-modified"
assert_file_has_content_literal summary.txt "Timestamp (ostree.commit.timestamp): "
assert_file_has_content_literal summary.txt "Version (ostree.commit.version): 3.2"
if has_ostree_feature gpgme; then
    assert_file_has_content_literal summary.txt "Good signature from"
fi
echo "ok view summary"

# Check the summary can be viewed raw too, but that it doesn’t include signature information.
${OSTREE} summary ${SUMMARY_HOMEDIR} --raw > raw-summary.txt
assert_file_has_content_literal raw-summary.txt "('main', ("
assert_file_has_content_literal raw-summary.txt "('other', ("
assert_file_has_content_literal raw-summary.txt "'ostree.summary.last-modified': <uint64"
assert_not_file_has_content raw-summary.txt "Found [0-9]+ signature"
assert_not_file_has_content raw-summary.txt "Summary is unsigned"
echo "ok view summary raw"
