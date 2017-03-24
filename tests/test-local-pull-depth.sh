#!/bin/bash
#
# Copyright (C) 2015 Dan Nicholson <nicholson@endlessm.com>
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

setup_test_repository "archive-z2"

echo "1..1"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2 --mode="archive-z2"

${CMD_PREFIX} ostree --repo=repo2 pull-local repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=0 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=1 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=1 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

${CMD_PREFIX} ostree --repo=repo2 pull-local --depth=-1 repo
find repo2/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

echo "ok local pull depth"
