#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"
setup_fake_remote_repo1 "archive-z2" "${COMMIT_SIGN}"

# Now, setup multiple branches
mkdir ${test_tmpdir}/ostree-srv/other-files
cd ${test_tmpdir}/ostree-srv/other-files
echo 'hello world another object' > hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b other -s "A commit" -m "Another Commit body"

mkdir ${test_tmpdir}/ostree-srv/yet-other-files
cd ${test_tmpdir}/ostree-srv/yet-other-files
echo 'hello world yet another object' > yet-another-hello-world
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b yet-another -s "A commit" -m "Another Commit body"

${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u

prev_dir=`pwd`
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo init --mode=archive-z2
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --mirror origin
assert_has_file repo/summary
${CMD_PREFIX} ostree --repo=repo checkout -U main main-copy
assert_file_has_content main-copy/baz/cow "moo"
${CMD_PREFIX} ostree --repo=repo checkout -U other other-copy
assert_file_has_content other-copy/hello-world "hello world another object"
${CMD_PREFIX} ostree --repo=repo checkout -U yet-another yet-another-copy
assert_file_has_content yet-another-copy/yet-another-hello-world "hello world yet another object"
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull mirror summary"

if ! ${CMD_PREFIX} ostree --version | grep -q -e '\+gpgme'; then
    exit 0;
fi

cd $prev_dir

${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

repo_reinit () {
  cd ${test_tmpdir}
  rm -rf repo
  mkdir repo
  ${OSTREE} --repo=repo init --mode=archive-z2
  ${OSTREE} --repo=repo remote add --set=gpg-verify-summary=true origin $(cat httpd-address)/ostree/gnomerepo
}

cd ${test_tmpdir}
repo_reinit
${OSTREE} --repo=repo pull origin main
echo "ok pull with signed summary"

cd ${test_tmpdir}
repo_reinit
mv ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{,.good}
echo invalid > ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig
if ${OSTREE} --repo=repo pull origin main 2>err.txt; then
    assert_not_reached "Successful pull with invalid GPG sig"
fi
assert_file_has_content err.txt "no signatures found"
mv ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.good,}
echo "ok pull with invalid summary gpg signature fails"

cd ${test_tmpdir}
repo_reinit
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{,.good}
# Some leading garbage
(echo invalid && cat ${test_tmpdir}/ostree-srv/gnomerepo/summary) > summary.bad.tmp && mv summary.bad.tmp ${test_tmpdir}/ostree-srv/gnomerepo/summary
if ${OSTREE} --repo=repo pull origin main; then
    assert_not_reached "Successful pull with invalid summary"
fi
mv ${test_tmpdir}/ostree-srv/gnomerepo/summary{.good,}
echo "ok pull with invalid summary (leading garbage) fails"

# Generate a delta
${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo static-delta generate --empty main
${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

cd ${test_tmpdir}
repo_reinit
${OSTREE} --repo=repo pull origin main
echo "ok pull delta with signed summary"

# Verify 'ostree remote summary' output.
${OSTREE} --repo=repo remote summary origin > summary.txt
assert_file_has_content summary.txt "* main"
assert_file_has_content summary.txt "* other"
assert_file_has_content summary.txt "* yet-another"
assert_file_has_content summary.txt "found 1 signature"
assert_file_has_content summary.txt "Good signature from \"Ostree Tester <test@test.com>\""
grep static-deltas summary.txt > static-deltas.txt
assert_file_has_content static-deltas.txt \
  $(${OSTREE} --repo=repo rev-parse origin:main)
