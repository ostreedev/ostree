#!/bin/bash
#
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

skip_without_user_xattrs

setup_fake_remote_repo1 "archive-z2"

echo '1..2'

cd ${test_tmpdir}
mkdir repo
${CMD_PREFIX} ostree --repo=repo init
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit --branch=test -m test -s test tree
done


${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=2 -v
find repo | grep \.commit$ | wc -l > commitcount
assert_file_has_content commitcount "^3$"
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=1 -v
find repo | grep \.commit$ | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"

${CMD_PREFIX} ostree --repo=repo fsck --add-tombstones
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content repo/config "tombstone-commits=true"
assert_file_has_content tombstonecommitcount "^1$"

# pull once again and use tombstone commits
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test

${CMD_PREFIX} ostree --repo=repo fsck --add-tombstones
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=0 -v
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_not_file_has_content tombstonecommitcount "^0$"

# and that tombstone are deleted once the commits are pulled again
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"

COMMIT_TO_DELETE=$(${CMD_PREFIX} ostree --repo=repo log test | grep ^commit | cut -f 2 -d' ' | tail -n 1)
${CMD_PREFIX} ostree --repo=repo prune --delete-commit=$COMMIT_TO_DELETE
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^1$"

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=0 -v
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="2005-10-29 12:43:29 +0000"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="2010-10-29 12:43:29 +0000"
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^3$"
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="2015-10-29 12:43:29 +0000"
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"


${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=0 -v
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 25 1985"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 21 2015"
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^4$"
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago"
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

${CMD_PREFIX} ostree --repo=repo commit --branch=oldcommit tree --timestamp="2005-10-29 12:43:29 +0000"
oldcommit_rev=$($OSTREE --repo=repo rev-parse oldcommit)
$OSTREE ls ${oldcommit_rev}
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago"
$OSTREE ls ${oldcommit_rev}

${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="November 05 1955"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 25 1985"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 21 2015"

${CMD_PREFIX} ostree --repo=repo static-delta generate test^
${CMD_PREFIX} ostree --repo=repo static-delta generate test
${CMD_PREFIX} ostree --repo=repo static-delta list | wc -l > deltascount
assert_file_has_content deltascount "^2$"
COMMIT_TO_DELETE=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)
${CMD_PREFIX} ostree --repo=repo prune --static-deltas-only --delete-commit=$COMMIT_TO_DELETE
${CMD_PREFIX} ostree --repo=repo static-delta list | wc -l > deltascount
assert_file_has_content deltascount "^1$"
${CMD_PREFIX} ostree --repo=repo static-delta generate test
${CMD_PREFIX} ostree --repo=repo static-delta list | wc -l > deltascount
assert_file_has_content deltascount "^2$"
${CMD_PREFIX} ostree --repo=repo prune --static-deltas-only --keep-younger-than="October 20 2015"
${CMD_PREFIX} ostree --repo=repo static-delta list | wc -l > deltascount
assert_file_has_content deltascount "^1$"

echo "ok prune"

rm repo -rf
${CMD_PREFIX} ostree --repo=repo init --mode=bare-user
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 --commit-metadata-only origin test
${CMD_PREFIX} ostree --repo=repo prune

echo "ok prune with partial repo"
