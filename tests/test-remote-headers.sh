#!/bin/bash
#
# Copyright (C) 2016 Red Hat, Inc.
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

echo '1..2'

. $(dirname $0)/libtest.sh

V=$($CMD_PREFIX ostree --version | \
  python3 -c 'import sys, yaml; print(yaml.safe_load(sys.stdin)["libostree"]["Version"])')

setup_fake_remote_repo1 "archive" "" \
  --expected-header foo=bar \
  --expected-header baz=badger \
  --expected-header "User-Agent=libostree/$V dodo/2.15"

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

# without proper User-Agent, the pull should fail
assert_fail ${CMD_PREFIX} ostree --repo=repo pull origin main \
  --http-header foo=bar \
  --http-header baz=badger
assert_fail ${CMD_PREFIX} ostree --repo=repo pull origin main \
  --http-header foo=bar \
  --http-header baz=badger \
  --append-user-agent bar/1.2

echo "ok setup done"

# Now pull should succeed now
${CMD_PREFIX} ostree --repo=repo pull \
  --http-header foo=bar \
  --http-header baz=badger \
  --append-user-agent dodo/2.15 \
  origin main

echo "ok pull succeeded"
