#!/bin/bash
#
# Copyright (C) 2015 Red Hat, Inc.
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

${CMD_PREFIX} ostree --repo=repo pull --commit-metadata-only origin main
find repo/objects -name '*.commit' | wc -l > commitcount
assert_file_has_content commitcount "^1$"
${CMD_PREFIX} ostree --repo=repo fsck

find repo/objects -name '*.file.*' | wc -l > commitcount
assert_file_has_content commitcount "^0$"

find repo/objects -name '*.dirtree' | wc -l > commitcount
assert_file_has_content commitcount "^0$"

echo "ok pull commit metadata only"
