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
if has_gpgme; then
    COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"
fi

setup_fake_remote_repo1 "archive" "${COMMIT_SIGN}"

# Set up a second branch.
mkdir ${test_tmpdir}/ostree-srv/other-files
cd ${test_tmpdir}/ostree-srv/other-files
echo 'hello world some object' > hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b other -s "A commit" -m "Example commit body"

# Generate the summary file.
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u

# Check out the repository.
prev_dir=`pwd`
cd ${test_tmpdir}
ostree_repo_init repo --mode=archive
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --mirror origin

# Check the summary file exists in the checkout, and can be viewed.
assert_has_file repo/summary
${OSTREE} summary --view > summary.txt
assert_file_has_content_literal summary.txt "* main"
assert_file_has_content_literal summary.txt "* other"
assert_file_has_content_literal summary.txt "ostree.summary.last-modified"
assert_file_has_content_literal summary.txt "Timestamp (ostree.commit.timestamp): "
assert_file_has_content_literal summary.txt "Version (ostree.commit.version): 3.2"
echo "ok view summary"

# Check the summary can be viewed raw too.
${OSTREE} summary --raw > raw-summary.txt
assert_file_has_content_literal raw-summary.txt "('main', ("
assert_file_has_content_literal raw-summary.txt "('other', ("
assert_file_has_content_literal raw-summary.txt "'ostree.summary.last-modified': <uint64"
echo "ok view summary raw"
