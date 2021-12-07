#!/bin/bash
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

setup_fake_remote_repo1 "archive" "" "--require-basic-auth"

echo '1..3'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

cd ${test_tmpdir}
rm repo -rf
ostree_repo_init repo
unauthaddress=$(cat httpd-address)
badauthaddress=$(echo $unauthaddress | sed -e 's,http://,http://foo:bar@,')
goodauthaddress=$(echo $unauthaddress | sed -e 's,http://,http://foouser:barpw@,')
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin-unauth ${unauthaddress}/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin-badauth ${badauthaddress}/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin-goodauth ${goodauthaddress}/ostree/gnomerepo

if ${CMD_PREFIX} ostree --repo=repo pull origin-unauth main 2>err.txt; then
    fatal "Pulled via unauth"
fi
assert_file_has_content err.txt "401"
echo "ok unauth"
rm -f err.txt
if ${CMD_PREFIX} ostree --repo=repo pull origin-badauth main 2>err.txt; then
    fatal "Pulled via badauth"
fi
assert_file_has_content err.txt "401"
rm -f err.txt
echo "ok badauth"

${CMD_PREFIX} ostree --repo=repo pull origin-goodauth main
echo "ok basic auth"
