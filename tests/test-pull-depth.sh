#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

setup_fake_remote_repo1 "archive"

echo '1..3'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

${CMD_PREFIX} ostree --repo=repo pull --depth=0 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo pull --depth=0 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo pull --depth=1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo pull --depth=1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^3$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

echo "ok pull depth"

# Check that pulling with depth != 0 succeeds with a missing parent
# commit. Prune the remote to truncate the history.
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo prune --refs-only --depth=0

rm -rf repo/refs/heads/* repo/refs/remotes/* repo/objects/*/*.commit
${CMD_PREFIX} ostree --repo=repo pull --depth=1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

rm -rf repo/refs/heads/* repo/refs/remotes/* repo/objects/*/*.commit
${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
find repo/state -name '*.commitpartial' | wc -l > commitpartialcount
assert_file_has_content commitpartialcount "^0$"

echo "ok pull depth missing parent"

# Check that it errors if the ref head commit is missing.
cd ${test_tmpdir}
rm -f ostree-srv/gnomerepo/objects/*/*.commit

rm -rf repo/refs/heads/* repo/refs/remotes/* repo/objects/*/*.commit
if ${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin main; then
    fatal "Pull with depth -1 succeeded with missing HEAD commit"
fi

echo "ok pull depth missing HEAD commit"
