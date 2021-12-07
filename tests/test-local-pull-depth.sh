#!/bin/bash
#
# Copyright (C) 2015 Dan Nicholson <nicholson@endlessm.com>
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_test_repository "archive"

echo "1..3"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2 --mode="archive"

${CMD_PREFIX} ostree --repo=repo2 pull-local repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo2/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=0 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo2/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=1 --commit-metadata-only repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo2/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^1$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=1 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo2/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=-1 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo2/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

echo "ok local pull depth"

# Check that pulling with depth != 0 succeeds with a missing parent
# commit. Prune the remote to truncate the history.
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo prune --refs-only --depth=0

rm -rf repo2/refs/heads/* repo2/refs/remotes/* repo2/objects/*/*.commit
${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=1 repo
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

rm -rf repo2/refs/heads/* repo2/refs/remotes/* repo2/objects/*/*.commit
${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=-1 repo
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

echo "ok local pull depth missing parent"

# Check that it errors if the ref head commit is missing.
cd ${test_tmpdir}
rm -f repo/objects/*/*.commit

rm -rf repo2/refs/heads/* repo2/refs/remotes/* repo2/objects/*/*.commit
if ${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=-1 repo; then
    fatal "Local pull with depth -1 succeeded with missing HEAD commit"
fi

echo "ok local pull depth missing HEAD commit"
