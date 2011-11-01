#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#
# Author: Colin Walters <walters@verbum.org>

set -e

echo "1..7"

. libtest.sh

setup_test_repository "regular"
echo "ok setup"

assert_file_has_content ${test_tmpdir}/repo/objects/f2/c7a70e5e252c1cb7a018d98f34cbf01d553185e9adc775e10213f4187fa91a.file moo
assert_streq "$(readlink ${test_tmpdir}/repo/objects/cf/443bea5eb400bc25b376c07925cc06cd05235d5fb38f20cd5f7ca53b7b3b10.file)" nonexistent

echo "ok check"

ostree checkout $ot_repo test2 checkout-test2
echo "ok checkout"

cd checkout-test2
assert_has_file firstfile
assert_has_file baz/cow
assert_file_has_content baz/cow moo
assert_has_file baz/deeper/ohyeah
echo "ok content"

ostree commit $ot_repo -b test2 -s delete -r firstfile
assert_has_file firstfile  # It should still exist in this checkout
cd $test_tmpdir
ostree checkout $ot_repo test2 $test_tmpdir/checkout-test2-2
cd $test_tmpdir/checkout-test2-2
assert_not_has_file firstfile
assert_has_file baz/saucer
echo "ok removal"

mkdir -p a/nested/tree
echo one > a/nested/tree/1
echo two2 > a/nested/2
echo 3 > a/nested/3
touch a/4
echo fivebaby > a/5
touch a/6
echo whee > 7
mkdir -p another/nested/tree
echo anotherone > another/nested/tree/1
echo whee2 > another/whee
# FIXME - remove grep for .
find | grep -v '^\.$' | ostree commit $ot_repo -b test2 -s "From find" --from-stdin
echo "ok stdin commit"

cd ${test_tmpdir}
ostree checkout $ot_repo test2 $test_tmpdir/checkout-test2-3
cd checkout-test2-3
assert_has_file a/nested/2
assert_file_has_content a/nested/2 'two2'
echo "ok stdin contents"
