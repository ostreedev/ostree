#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive"

echo '1..1'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

${CMD_PREFIX} ostree --repo=repo pull --depth=0 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"

${CMD_PREFIX} ostree --repo=repo pull --depth=0 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"

${CMD_PREFIX} ostree --repo=repo pull --depth=1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

${CMD_PREFIX} ostree --repo=repo pull --depth=1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^2$"

${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^3$"

echo "ok pull depth"
