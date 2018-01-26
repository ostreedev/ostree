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
rm repo -rf
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

${CMD_PREFIX} ostree --repo=repo pull origin main --subpath /baz
${CMD_PREFIX} ostree fsck --repo=repo >fsck.out
assert_file_has_content fsck.out 'Verifying content integrity of 0 commit objects'
assert_file_has_content fsck.out '1 partial commits not verified'

echo "ok"
