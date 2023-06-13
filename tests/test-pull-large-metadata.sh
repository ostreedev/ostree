#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive"

echo '1..1'

# Overwrite the commit object with 121 M of zeroes. This is based on the
# OSTREE_MAX_METADATA_SIZE constant in src/libostree/ostree-core.h
cd ${test_tmpdir}
rev=$(cd ostree-srv && ${CMD_PREFIX} ostree --repo=gnomerepo rev-parse main)
dd if=/dev/zero bs=1M count=130 of=ostree-srv/gnomerepo/objects/$(echo $rev | cut -b 1-2)/$(echo $rev | cut -b 3-).commit

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>pulllog.txt 1>&2; then
    assert_not_reached "pull unexpectedly succeeded!"
fi
assert_file_has_content pulllog.txt "exceeded maximum"

echo "ok"
