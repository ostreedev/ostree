#!/bin/bash
#
# Copyright © 2019 Wind River Systems, Inc.
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

echo '1..6'

cd ${test_tmpdir}

rm -rf ./f1
mkdir -p ./f1
${CMD_PREFIX} ostree --repo=./f1 init --mode=archive-z2
rm -rf ./trial
mkdir -p ./trial
echo test > ./trial/test
${CMD_PREFIX} ostree --repo=./f1 commit --tree=dir=./trial --skip-if-unchanged --branch=exp1 --subject="test Commit"

rm -rf ./f2
mkdir -p ./f2
${CMD_PREFIX} ostree --repo=./f2 init --mode=archive-z2
${CMD_PREFIX} ostree --repo=./f2 pull-local  ./f1
echo "ok 1 fsck-pre-commit"

file=`find ./f2 |grep objects |grep \\.file |tail -1 `
rm $file
echo whoops > $file

# First check for corruption
if ${CMD_PREFIX} ostree fsck --repo=./f2 > fsck 2> fsck-error; then
  assert_not_reached "fsck did not fail"
fi
assert_file_has_content fsck "^Validating refs\.\.\.$"
assert_file_has_content fsck-error "^error: In commits"

echo "ok 2 fsck-fail-check"

# Fix the corruption
if ${CMD_PREFIX} ostree fsck --delete --repo=./f2 > fsck 2> fsck-error; then
  assert_not_reached "fsck did not fail"
fi
assert_file_has_content fsck "^Validating refs\.\.\.$"
assert_file_has_content fsck-error "^In commits"

echo "ok 3 fsck-delete-check"

# Check that fsck still exits with non-zero after corruption fix
if ${CMD_PREFIX} ostree fsck --repo=./f2 > fsck 2> fsck-error; then
  assert_not_reached "fsck did not fail"
fi
assert_file_has_content fsck "^Validating refs\.\.\.$"
assert_file_has_content fsck-error "^error: 1"

echo "ok 4 fsck-post-delete-check"

${CMD_PREFIX} ostree --repo=./f2 pull-local ./f1 > /dev/null
echo "ok 5 fsck-repair"

if ! ${CMD_PREFIX} ostree fsck --repo=./f2 > fsck 2> fsck-error; then
  assert_not_reached "fsck failed when it should have passed"
fi
assert_file_has_content fsck "^Validating refs\.\.\.$"
assert_file_empty fsck-error
echo "ok 6 fsck-good"
