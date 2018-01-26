#!/bin/bash
#
# Copyright (C) 2016 Red Hat
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"
setup_fake_remote_repo1 "archive" "${COMMIT_SIGN}" --random-500s=50

cd ${test_tmpdir}
ostree_repo_init repo --mode=archive
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
for x in $(seq 200); do
    if ${CMD_PREFIX} ostree --repo=repo pull --mirror origin main 2>err.txt; then
	echo "Success on iteration ${x}"
	break;
    fi
    assert_file_has_content err.txt "\(500.*Internal Server Error\)\|\(HTTP 500\)"
done

${CMD_PREFIX} ostree --repo=repo fsck
${CMD_PREFIX} ostree --repo=repo rev-parse main

echo "ok repeated pull after 500s"
