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

setup_fake_remote_repo1 "archive-z2"

echo '1..1'

cd ${test_tmpdir}
mkdir repo
${CMD_PREFIX} ostree --repo=repo init

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
