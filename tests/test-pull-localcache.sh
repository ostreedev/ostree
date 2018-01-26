#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive"

echo '1..2'

cd ${test_tmpdir}
gnomerepo_url="$(cat httpd-address)/ostree/gnomerepo"

# Set up our local cache
ostree_repo_init repo-local --mode "archive"
${CMD_PREFIX} ostree --repo=repo-local remote add --set=gpg-verify=false origin ${gnomerepo_url}

init_repo() {
  ostree_repo_init repo --mode "archive"
  ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin ${gnomerepo_url}
}

# Pull the contents to our local cache
${CMD_PREFIX} ostree --repo=repo-local pull origin main
rm files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main files
echo anewfile > files/anewfile
commit=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main --tree=dir=files)

# Check that pulling a new commit of the same tree hits our cache
rm -rf repo
init_repo
${CMD_PREFIX} ostree --repo=repo pull --localcache-repo repo-local origin main >out.txt
assert_file_has_content out.txt '3 metadata, 1 content objects fetched (4 meta, 5 content local)'
echo "ok pull --localcache-repo"

# Check that pulling the same commit works as well
rm -rf repo
init_repo
${CMD_PREFIX} ostree --repo=repo-local pull origin $commit
${CMD_PREFIX} ostree --repo=repo pull -L repo-local origin main
commit2=$(${CMD_PREFIX} ostree --repo=repo rev-parse main)
[[ $commit == $commit2 ]]
# and check that it's not partial
rm -rf files
${CMD_PREFIX} ostree --repo=repo checkout main files
test -f files/anewfile
echo "ok pull --localcache-repo same commit"
