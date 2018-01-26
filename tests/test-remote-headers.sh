#!/bin/bash
#
# Copyright (C) 2016 Red Hat, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

echo '1..2'

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive" "" \
  "--expected-header foo=bar --expected-header baz=badger"

assert_fail (){
  set +e
  $@
  if [ $? = 0 ] ; then
    echo 1>&2 "$@ did not fail"; exit 1
  fi
  set -euo pipefail
}

cd ${test_tmpdir}
rm repo -rf
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

# Sanity check the setup, without headers the pull should fail
assert_fail ${CMD_PREFIX} ostree --repo=repo pull origin main

echo "ok setup done"

# Now pull should succeed now
${CMD_PREFIX} ostree --repo=repo pull --http-header foo=bar --http-header baz=badger origin main

echo "ok pull succeeded"
