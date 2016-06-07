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

setup_fake_remote_repo1 "archive-z2"

echo '1..2'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

for remoteurl in $(cat httpd-address)/ostree/gnomerepo \
		 file://$(pwd)/ostree-srv/gnomerepo; do
cd ${test_tmpdir}
rm repo -rf
mkdir repo
${CMD_PREFIX} ostree --repo=repo init
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin ${remoteurl}

${CMD_PREFIX} ostree --repo=repo pull --subpath=/baz origin main

${CMD_PREFIX} ostree --repo=repo ls origin:main /baz
if ${CMD_PREFIX} ostree --repo=repo ls origin:main /firstfile 2>err.txt; then
    assert_not_reached
fi
assert_file_has_content err.txt "Couldn't find file object"
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_has_file repo/state/${rev}.commitpartial

# Test pulling a file, not a dir
${CMD_PREFIX} ostree --repo=repo pull --subpath=/firstfile origin main
${CMD_PREFIX} ostree --repo=repo ls origin:main /firstfile

${CMD_PREFIX} ostree --repo=repo pull origin main
assert_not_has_file repo/state/${rev}.commitpartial
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok"
done
