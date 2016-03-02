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

echo "1..5"

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
rev=$(ostree --repo=repo rev-parse main)
find repo/objects -name '*.filez' | while read name; do
    mode=$(stat -c '%a' "${name}")
    if test "${mode}" = 600; then
	assert_not_reached "Content object unreadable by others: ${mode}"
    fi
done
echo "ok pull mirror summary"

if ! ${CMD_PREFIX} ostree --version | grep -q -e '\+gpgme'; then
    exit 0;
fi

cd $prev_dir

cd ${test_tmpdir}
rm -rf repo
mkdir repo
${OSTREE} --repo=repo init --mode=archive-z2
${OSTREE} --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
echo "ok pull mirror without checking signed summary"

cd ${test_tmpdir}
rm -rf repo
mkdir repo
${OSTREE} --repo=repo init --mode=archive-z2
${OSTREE} --repo=repo remote add --set=gpg-verify-summary=true origin $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} --repo=repo pull --mirror origin 2>err.txt; then
    assert_not_reached "Mirroring unexpectedly succeeded"
fi
echo "ok pull mirror without signed summary"

${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

cd ${test_tmpdir}
rm -rf repo
mkdir repo
${OSTREE} --repo=repo init --mode=archive-z2
${OSTREE} --repo=repo remote add --set=gpg-verify-summary=true origin $(cat httpd-address)/ostree/gnomerepo
${OSTREE} --repo=repo pull --mirror origin
assert_has_file repo/summary
assert_has_file repo/summary.sig
echo "ok pull mirror with signed summary"

cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{,.good}
truncate --size=1 ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig

cd ${test_tmpdir}
rm -rf repo
mkdir repo
${OSTREE} --repo=repo init --mode=archive-z2
${OSTREE} --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
${OSTREE} --repo=repo pull --mirror origin
assert_has_file repo/summary
assert_has_file repo/summary.sig
mv ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.good,}
echo "ok pull mirror with invalid summary sig and no verification"

# Uncomment when we support mirroring deltas
#
# ${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo static-delta generate main
# origmain=$(ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo rev-parse main^)
# newmain=$(ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo rev-parse main)
# ${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}
# ${OSTREE} --repo=repo pull --mirror origin
# ${OSTREE} --repo=repo static-delta list >deltas.txt
# assert_file_has_content deltas.txt "${origmain}-${newmain}"

# echo "ok pull mirror with signed summary covering static deltas"
