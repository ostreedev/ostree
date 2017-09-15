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

setup_fake_remote_repo1 "archive"

echo '1..12'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit --branch=test -m test -s test tree
done


${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test

assert_repo_has_n_commits() {
    repo=$1
    count=$2
    assert_streq "$(find ${repo}/objects -name '*.commit' | wc -l)" "${count}"
}

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=2 -v
assert_repo_has_n_commits repo 3
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"
$OSTREE fsck

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=1 -v
assert_repo_has_n_commits repo 2
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
assert_repo_has_n_commits repo 1
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_not_file_has_content tombstonecommitcount "^0$"
$OSTREE fsck

# and that tombstone are deleted once the commits are pulled again
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin test
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^0$"

COMMIT_TO_DELETE=$(${CMD_PREFIX} ostree --repo=repo log test | grep ^commit | cut -f 2 -d' ' | tail -n 1)
${CMD_PREFIX} ostree --repo=repo prune --delete-commit=$COMMIT_TO_DELETE
find repo/objects -name '*.tombstone-commit' | wc -l > tombstonecommitcount
assert_file_has_content tombstonecommitcount "^1$"
$OSTREE fsck

${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=0 -v
assert_repo_has_n_commits repo 1
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="2005-10-29 12:43:29 +0000"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="2010-10-29 12:43:29 +0000"
assert_repo_has_n_commits repo 3
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="2015-10-29 12:43:29 +0000"
assert_repo_has_n_commits repo 2
$OSTREE fsck


${CMD_PREFIX} ostree prune --repo=repo --refs-only --depth=0 -v
assert_repo_has_n_commits repo 2
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 25 1985"
${CMD_PREFIX} ostree --repo=repo commit --branch=test -m test -s test tree --timestamp="October 21 2015"
assert_repo_has_n_commits repo 4
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago"
assert_repo_has_n_commits repo 2

${CMD_PREFIX} ostree --repo=repo commit --branch=oldcommit tree --timestamp="2005-10-29 12:43:29 +0000"
oldcommit_rev=$($OSTREE --repo=repo rev-parse oldcommit)
$OSTREE ls ${oldcommit_rev}
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago"
$OSTREE ls ${oldcommit_rev}
$OSTREE fsck

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
${CMD_PREFIX} ostree --repo=repo fsck
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
ostree_repo_init repo --mode=bare-user
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 --commit-metadata-only origin test
${CMD_PREFIX} ostree --repo=repo prune

echo "ok prune with partial repo"

assert_has_n_objects() {
    find $1/objects -name '*.filez' | wc -l > object-count
    assert_file_has_content object-count $2
    rm object-count
}

cd ${test_tmpdir}
for repo in repo child-repo tmp-repo; do
    rm ${repo} -rf
    ostree_repo_init ${repo} --mode=archive
done
echo parent=${test_tmpdir}/repo >> child-repo/config
mkdir files
for x in $(seq 3); do
    echo afile${x} > files/afile${x}
done
${CMD_PREFIX} ostree --repo=repo commit -b test files
assert_has_n_objects repo 3
# Inherit 1-3, add 4-6
for x in $(seq 4 6); do
    echo afile${x} > files/afile${x}
done
# Commit into a temp repo, then do a local pull, which triggers
# the parent repo lookup for dedup
${CMD_PREFIX} ostree --repo=tmp-repo commit -b childtest files
${CMD_PREFIX} ostree --repo=child-repo pull-local tmp-repo childtest
assert_has_n_objects child-repo 3
# Sanity check prune doesn't do anything
for repo in repo child-repo; do ${CMD_PREFIX} ostree --repo=${repo} prune; done
# Now, leave orphaned objects in the parent only pointed to by the child
${CMD_PREFIX} ostree --repo=repo refs --delete test
${CMD_PREFIX} ostree --repo=child-repo prune --refs-only --depth=0
assert_has_n_objects child-repo 3

echo "ok prune with parent repo"

# Delete all the above since I can't be bothered to think about how new tests
# would interact. We make a new repo test suite, then clone it
# for "subtests" below with reinitialize_datesnap_repo()
rm repo datetest-snapshot-repo -rf
ostree_repo_init datetest-snapshot-repo --mode=archive
# Some ancient commits on the both a stable/dev branch
for day in $(seq 5); do
    ${CMD_PREFIX} ostree --repo=datetest-snapshot-repo commit --branch=stable -m test -s "old stable build $day" tree --timestamp="October $day 1985"
    ${CMD_PREFIX} ostree --repo=datetest-snapshot-repo commit --branch=dev -m test -s "old dev build $day" tree --timestamp="October $day 1985"
done
# And some new ones
for x in $(seq 3); do
    ${CMD_PREFIX} ostree --repo=datetest-snapshot-repo commit --branch=stable -m test -s "new stable build $x" tree
    ${CMD_PREFIX} ostree --repo=datetest-snapshot-repo commit --branch=dev -m test -s "new dev build $x" tree
done
assert_repo_has_n_commits datetest-snapshot-repo 16

# Snapshot the above
reinitialize_datesnap_repo() {
    rm repo -rf
    ostree_repo_init repo --mode=archive
    ${CMD_PREFIX} ostree --repo=repo pull-local --depth=-1 datetest-snapshot-repo 
}

# This test prunes with both younger than as well as a full strong ref to the
# stable branch
reinitialize_datesnap_repo
# First, a quick test of invalid input
if ${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago" --retain-branch-depth=stable=BACON 2>err.txt; then
    assert_not_reached "BACON is a number?!"
fi
assert_file_has_content err.txt 'Invalid depth BACON'
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago" --retain-branch-depth=stable=-1
assert_repo_has_n_commits repo 11
# Double check our backup is unchanged
assert_repo_has_n_commits datetest-snapshot-repo 16
$OSTREE fsck

# Again but this time only retain 6 (5+1) commits on stable.  This should drop
# out 8 - 6 = 2 commits (so the 11 above minus 2 = 9)
${CMD_PREFIX} ostree --repo=repo prune --keep-younger-than="1 week ago" --retain-branch-depth=stable=5
assert_repo_has_n_commits repo 9
$OSTREE fsck
echo "ok retain branch depth and keep-younger-than"

# Just stable branch ref, we should prune everything except the tip of dev,
# so 8 stable + 1 dev = 9
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --depth=0 --retain-branch-depth=stable=-1
assert_repo_has_n_commits repo 9
$OSTREE fsck

echo "ok retain branch depth (alone)"

# Test --only-branch with --depth=0; this should be exactly identical to the
# above with a result of 9.
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --depth=0
assert_repo_has_n_commits repo 9
$OSTREE fsck
echo "ok --only-branch --depth=0"

# Test --only-branch with --depth=1; should just add 1 to the above, for 10.
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --depth=1
assert_repo_has_n_commits repo 10
echo "ok --only-branch --depth=1"

# Test --only-branch with all branches
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --only-branch=stable --depth=0
assert_repo_has_n_commits repo 2
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --only-branch=stable --depth=1
assert_repo_has_n_commits repo 4
echo "ok --only-branch (all) --depth=1"

# Test --only-branch and --retain-branch-depth overlap
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --only-branch=stable --depth=0 \
                     --retain-branch-depth=stable=2
assert_repo_has_n_commits repo 4
echo "ok --only-branch and --retain-branch-depth overlap"

# Test --only-branch and --retain-branch-depth together
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=dev --depth=0 --retain-branch-depth=stable=2
assert_repo_has_n_commits repo 4
echo "ok --only-branch and --retain-branch-depth together"

# Test --only-branch with --keep-younger-than; this should be identical to the test
# above for --retain-branch-depth=stable=-1
reinitialize_datesnap_repo
${CMD_PREFIX} ostree --repo=repo prune --only-branch=stable --keep-younger-than="1 week ago"
assert_repo_has_n_commits repo 11
echo "ok --only-branch --keep-younger-than"

# Test --only-branch with a nonexistent ref
reinitialize_datesnap_repo
if ${CMD_PREFIX} ostree --repo=repo prune --only-branch=BACON 2>err.txt; then
    fatal "we pruned BACON?"
fi
assert_file_has_content err.txt "Refspec.*BACON.*not found"
echo "ok --only-branch=BACON"
