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

echo "ok refs"
