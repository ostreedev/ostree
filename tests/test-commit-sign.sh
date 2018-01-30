#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
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

set -euo pipefail

. $(dirname $0)/libtest.sh

if ! has_gpgme; then
    echo "1..0 #SKIP no gpg support compiled in"
    exit 0
fi

echo "1..6"

keyid="472CDAFA"
oldpwd=`pwd`
mkdir ostree-srv
cd ostree-srv
mkdir gnomerepo
ostree_repo_init gnomerepo --mode="archive"
mkdir gnomerepo-files
cd gnomerepo-files 
echo first > firstfile
mkdir baz
echo moo > baz/cow
echo alien > baz/saucer
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "A remote commit" -m "Some Commit body" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
mkdir baz/deeper
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "Add deeper" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
echo hi > baz/deeper/ohyeah
mkdir baz/another/
echo x > baz/another/y
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "The rest" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
cd ..

cd ${test_tmpdir}
mkdir ${test_tmpdir}/httpd
cd httpd
ln -s ${test_tmpdir}/ostree-srv ostree
${OSTREE_HTTPD} --autoexit --daemonize -P 18081 -p ${test_tmpdir}/httpd-port
port=$(cat ${test_tmpdir}/httpd-port)
assert_streq $port 18081
echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
cd ${oldpwd} 

export OSTREE="${CMD_PREFIX} ostree --repo=repo"

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

# Set OSTREE_GPG_HOME to a place with no keyrings, we shouldn't trust the signature
cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
if env OSTREE_GPG_HOME=${test_tmpdir} ${CMD_PREFIX} ostree --repo=repo pull origin main; then
    assert_not_reached "pull with no trusted GPG keys unexpectedly succeeded!"
fi
rm repo -rf
echo "ok pull no trusted GPG"

# And a test case with valid signature
cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo show --gpg-verify-remote=origin main > show.txt
assert_file_has_content_literal show.txt 'Found 1 signature'
rm repo -rf
echo "ok pull verify"

# A test with corrupted detached signature
cd ${test_tmpdir}
find ${test_tmpdir}/ostree-srv/gnomerepo -name '*.commitmeta' | while read fname; do
    echo borkborkbork > ${fname};
done
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
    assert_not_reached "pull with corrupted signature unexpectedly succeeded!"
fi
rm repo -rf
echo "ok pull corrupted sig"

# And now attempt to pull the same corrupted commit, but with GPG
# verification off
cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main
rm repo -rf
echo "ok repull corrupted"

# Add an unsigned commit to the repo, then pull, then sign the commit,
# then pull again.  Make sure we get the expected number of signatures
# each time.
cd ${test_tmpdir}/ostree-srv/gnomerepo-files
echo secret > signme
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "Don't forget to sign me!"
cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo show main > show.txt
assert_not_file_has_content show.txt 'Found.*signature'
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo gpg-sign --gpg-homedir=${test_tmpdir}/gpghome main $keyid
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo show main > show.txt
assert_file_has_content_literal show.txt 'Found 1 signature'
echo "ok pull unsigned, then sign"

# Delete the signature from the commit so the detached metadata is empty,
# then pull and verify the signature is also deleted on the client side.
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo gpg-sign --gpg-homedir=${test_tmpdir}/gpghome --delete main $keyid
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo show main >show.txt
assert_not_file_has_content show.txt 'Found.*signature'
echo "ok pull sig deleted"

rm -rf repo gnomerepo-files
libtest_cleanup_gpg
