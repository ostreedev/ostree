#!/bin/bash
#
# Copyright (C) 2016 Endless Mobile, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive"

echo '1..1'

cd ${test_tmpdir}
# get a list of XX/XXXXXXX...XX.commit
find ostree-srv/gnomerepo/objects -name '*.commit' | cut -d/ -f4- | sort >${test_tmpdir}/original_commits


gnomerepo_url="$(cat httpd-address)/ostree/gnomerepo"
mkdir mirror-srv
cd mirror-srv
mkdir gnomerepo
ostree_repo_init gnomerepo --mode "archive"
${CMD_PREFIX} ostree --repo=gnomerepo remote add --set=gpg-verify=false origin ${gnomerepo_url}
${CMD_PREFIX} ostree --repo=gnomerepo pull --mirror --depth=-1 origin main

find gnomerepo/objects -name '*.commit' | cut -d/ -f3- | sort >${test_tmpdir}/mirror_commits

if ! cmp --quiet ${test_tmpdir}/original_commits ${test_tmpdir}/mirror_commits
then
    assert_not_reached 'original repository and its mirror should have the same commits'
fi

cd ${test_tmpdir}
mkdir mirror-httpd
cd mirror-httpd
ln -s ${test_tmpdir}/mirror-srv ostree
mirror_log="${test_tmpdir}/mirror_log"
${OSTREE_HTTPD} --log-file=${mirror_log} --autoexit --daemonize -p ${test_tmpdir}/mirror-httpd-port
port=$(cat ${test_tmpdir}/mirror-httpd-port)
echo "http://127.0.0.1:${port}" > ${test_tmpdir}/mirror-httpd-address

cd ${test_tmpdir}
mirrorrepo_url="$(cat mirror-httpd-address)/ostree/gnomerepo"
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin ${gnomerepo_url}
rm -rf ${test_tmpdir}/ostree-srv/gnomerepo
if ${CMD_PREFIX} ostree --repo=repo pull --depth=-1 origin main
then
    assert_not_reached 'pulling from removed remote should have failed'
fi

if ! ${CMD_PREFIX} ostree --repo=repo pull --depth=-1 --url=${mirrorrepo_url} origin main
then
    cat ${mirror_log}
    assert_not_reached 'fetching from the overridden URL should succeed'
fi
find repo/objects -name '*.commit' | cut -d/ -f3- | sort >${test_tmpdir}/repo_commits

if ! cmp --quiet ${test_tmpdir}/original_commits ${test_tmpdir}/repo_commits
then
    assert_not_reached 'original repository and its mirror should have the same commits'
fi

echo "ok pull url override"
