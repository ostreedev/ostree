#!/bin/bash
#
# Copyright (C) 2015 Dan Nicholson <nicholson@endlessm.com>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_test_repository "archive"

echo "1..1"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2 --mode="archive"

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
