#!/bin/bash
#
# Copyright (C) 2016 Red Hat, Inc.
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

setup_fake_remote_repo1 "archive"

echo '1..5'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read i; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=repo commit --branch=test-$i -m test -s test tree
    ${CMD_PREFIX} ostree --repo=repo commit --branch=foo/test-$i -m test -s test tree
done

${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount
assert_file_has_content refscount "^10$"

${CMD_PREFIX} ostree --repo=repo refs foo > refs
assert_not_file_has_content refs foo

${CMD_PREFIX} ostree --repo=repo refs --list foo > refs
assert_file_has_content refs foo

${CMD_PREFIX} ostree --repo=repo refs foo | wc -l > refscount.foo
assert_file_has_content refscount.foo "^5$"

${CMD_PREFIX} ostree --repo=repo refs --delete 2>/dev/null || true
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.delete1
assert_file_has_content refscount.delete1 "^10$"

${CMD_PREFIX} ostree refs --delete 2>/dev/null && (echo 1>&2 "refs --delete (without prefix) unexpectedly succeeded!"; exit 1)
${CMD_PREFIX} ostree --repo=repo refs --delete test-1 test-2
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.delete2
assert_file_has_content refscount.delete2 "^8$"

${CMD_PREFIX} ostree refs --repo=repo --delete foo
${CMD_PREFIX} ostree refs --repo=repo | wc -l > refscount.delete3
assert_file_has_content refscount.delete3 "^3$"
assert_not_file_has_content reflist '^test-1$'

#Add a few more commits, to test --create
${CMD_PREFIX} ostree --repo=repo commit --branch=ctest -m ctest -s ctest tree
${CMD_PREFIX} ostree --repo=repo commit --branch=foo/ctest -m ctest -s ctest tree

${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount
assert_file_has_content refscount "^5$"

if ${CMD_PREFIX} ostree --repo=repo refs --create=ctest-new; then
    assert_not_reached "refs --create unexpectedly succeeded without specifying an existing ref!"
fi
if ${CMD_PREFIX} ostree --repo=repo refs ctest --create; then
    assert_not_reached "refs --create unexpectedly succeeded without specifying the ref to create!"
fi
if ${CMD_PREFIX} ostree --repo=repo refs does-not-exist --create=ctest-new; then
    assert_not_reached "refs --create unexpectedly succeeded for a prefix that doesn't exist!"
fi
if ${CMD_PREFIX} ostree --repo=repo refs ctest --create=foo; then
    assert_not_reached "refs --create unexpectedly succeeded for a prefix that is already in use by a folder!"
fi
if ${CMD_PREFIX} ostree --repo=repo refs foo/ctest --create=ctest; then
    assert_not_reached "refs --create unexpectedly succeeded in overwriting an existing prefix!"
fi

# https://github.com/ostreedev/ostree/issues/1285
# One tool was creating .latest_rsync files in each dir, let's ignore stuff like
# that.
echo '👻' > repo/refs/heads/.spooky_and_hidden
${CMD_PREFIX} ostree --repo=repo refs > refs.txt
assert_not_file_has_content refs.txt 'spooky'
${CMD_PREFIX} ostree --repo=repo refs ctest --create=exampleos/x86_64/standard
echo '👻' > repo/refs/heads/exampleos/x86_64/.further_spooky_and_hidden
${CMD_PREFIX} ostree --repo=repo refs > refs.txt
assert_file_has_content refs.txt 'exampleos/x86_64/standard'
assert_not_file_has_content refs.txt 'spooky'
rm repo/refs/heads/exampleos -rf
echo "ok hidden refs"

for ref in '.' '-' '.foo' '-bar' '!' '!foo'; do
    if ${CMD_PREFIX} ostree --repo=repo refs ctest --create=${ref} 2>err.txt; then
        fatal "Created ref ${ref}"
    fi
    assert_file_has_content err.txt 'Invalid ref'
done
echo "ok invalid refs"

for ref in 'org.foo.bar/x86_64/standard-blah'; do
    ostree --repo=repo refs ctest --create=${ref}
    ostree --repo=repo rev-parse ${ref} >/dev/null
    ostree --repo=repo refs --delete ${ref}
done
echo "ok valid refs with chars '._-'"

#Check to see if any uncleaned tmp files were created after failed --create
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create1
assert_file_has_content refscount.create1 "^5$"

${CMD_PREFIX} ostree --repo=repo refs ctest --create=ctest-new
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create2
assert_file_has_content refscount.create2 "^6$"

#Check to see if a deleted folder can be re-used as the name of a ref
${CMD_PREFIX} ostree --repo=repo refs foo/ctest --delete
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create3
assert_file_has_content refscount.create3 "^5$"
${CMD_PREFIX} ostree --repo=repo refs ctest --create=foo
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create4
assert_file_has_content refscount.create4 "^6$"

#Check if a name for a ref in remote repo can be used locally, and vice versa.
${CMD_PREFIX} ostree --repo=repo commit --branch=origin:remote1
${CMD_PREFIX} ostree --repo=repo commit --branch=local1
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create5
assert_file_has_content refscount.create5 "^8$"

${CMD_PREFIX} ostree --repo=repo refs origin:remote1 --create=remote1
${CMD_PREFIX} ostree --repo=repo refs origin:remote1 --create=origin/remote1
${CMD_PREFIX} ostree --repo=repo refs local1 --create=origin:local1
${CMD_PREFIX} ostree --repo=repo refs | wc -l > refscount.create6
assert_file_has_content refscount.create6 "^11$"

echo "ok refs"

# Test symlinking a ref
${CMD_PREFIX} ostree --repo=repo refs ctest --create=exampleos/x86_64/26/server
${CMD_PREFIX} ostree --repo=repo refs -A exampleos/x86_64/26/server --create=exampleos/x86_64/stable/server
${CMD_PREFIX} ostree --repo=repo summary -u
${CMD_PREFIX} ostree --repo=repo refs > refs.txt
for v in 26 stable; do
    assert_file_has_content refs.txt exampleos/x86_64/${v}/server
done
${CMD_PREFIX} ostree --repo=repo refs -A > refs.txt
assert_file_has_content_literal refs.txt 'exampleos/x86_64/stable/server -> exampleos/x86_64/26/server'
assert_not_file_has_content refs.txt '^exampleos/x86_64/26/server'
stable=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/stable/server)
current=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/26/server)
assert_streq "${stable}" "${current}"
${CMD_PREFIX} ostree --repo=repo commit -b exampleos/x86_64/26/server --tree=dir=tree
${CMD_PREFIX} ostree --repo=repo summary -u
newcurrent=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/26/server)
assert_not_streq "${newcurrent}" "${current}"
newstable=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/stable/server)
assert_streq "${newcurrent}" "${newstable}"

# Test that we can swap the symlink
${CMD_PREFIX} ostree --repo=repo commit -b exampleos/x86_64/27/server --tree=dir=tree
newcurrent=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/27/server)
assert_not_streq "${newcurrent}" "${newstable}"
${CMD_PREFIX} ostree --repo=repo refs -A exampleos/x86_64/27/server --create=exampleos/x86_64/stable/server
newnewstable=$(${CMD_PREFIX} ostree --repo=repo rev-parse exampleos/x86_64/stable/server)
assert_not_streq "${newnewstable}" "${newstable}"
assert_streq "${newnewstable}" "${newcurrent}"
${CMD_PREFIX} ostree --repo=repo refs > refs.txt
for v in 26 27 stable; do
    assert_file_has_content refs.txt exampleos/x86_64/${v}/server
done
${CMD_PREFIX} ostree --repo=repo refs -A > refs.txt
assert_file_has_content_literal refs.txt 'exampleos/x86_64/stable/server -> exampleos/x86_64/27/server'

${CMD_PREFIX} ostree --repo=repo summary -u

echo "ok ref symlink"
