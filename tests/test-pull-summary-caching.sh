#!/bin/bash
#
# Copyright © 2020 Endless OS Foundation LLC
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
#
# Authors:
#  - Philip Withnall <pwithnall@endlessos.org>

set -euo pipefail

. $(dirname $0)/libtest.sh

if ! has_gpgme; then
    echo "1..0 #SKIP no gpg support compiled in"
    exit 0
fi

# Ensure repo caching is in use.
unset OSTREE_SKIP_CACHE

COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"

echo "1..1"

setup_fake_remote_repo2 "archive" "${COMMIT_SIGN}"

# Create a few branches and update the summary file (and sign it)
mkdir ${test_tmpdir}/ostree-srv/other-files
cd ${test_tmpdir}/ostree-srv/other-files
echo 'hello world another object' > hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b other -s "A commit" -m "Another Commit body"

mkdir ${test_tmpdir}/ostree-srv/yet-other-files
cd ${test_tmpdir}/ostree-srv/yet-other-files
echo 'hello world yet another object' > yet-another-hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b yet-another -s "A commit" -m "Another Commit body"

${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

# Test that pulling twice in a row doesn’t re-download the summary file or its signature
cd ${test_tmpdir}
rm -rf repo
ostree_repo_init repo --mode=archive
${OSTREE} --repo=repo remote add --set=gpg-verify-summary=true origin $(cat httpd-address)/ostree/gnomerepo
${OSTREE} --repo=repo pull origin other
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
summary_inode="$(stat -c '%i' repo/tmp/cache/summaries/origin)"
summary_sig_inode="$(stat -c '%i' repo/tmp/cache/summaries/origin.sig)"
${OSTREE} --repo=repo pull origin other
assert_streq "$(stat -c '%i' repo/tmp/cache/summaries/origin)" "${summary_inode}"
assert_streq "$(stat -c '%i' repo/tmp/cache/summaries/origin.sig)" "${summary_sig_inode}"
echo "ok pull caches the summary files"
