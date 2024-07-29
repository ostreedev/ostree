#!/bin/bash
#
# Copyright (C) 2024 Red Hat, Inc.
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

set -euox pipefail

. $(dirname $0)/libtest.sh

touch foo
if ! cp --reflink=always foo bar &>/dev/null; then
    skip "no reflinks"
fi
rm foo bar

$CMD_PREFIX ostree --repo=repo init --mode=bare-user
$CMD_PREFIX ostree config --repo=repo set core.payload-link-threshold 0

mkdir d
echo test > d/testcontent
chmod 0644 d/testcontent
$CMD_PREFIX ostree --repo=repo commit --tree=dir=d --consume -b testref
mkdir d
echo test > d/testcontent
chmod 0755 d/testcontent
$CMD_PREFIX ostree --repo=repo commit --tree=dir=d --consume -b testref
find repo -type l -name '*.payload-link' >payload-links.txt
assert_streq "$(wc -l < payload-links.txt)" "1"

tap_ok payload-link

tap_end
