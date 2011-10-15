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

. libtest.sh

echo '1..5'

setup_test_repository2
echo 'ok setup'
hacktree checkout $ht_repo HEAD $test_tmpdir/checkout2-head
echo 'ok checkout cmd'
cd $test_tmpdir/checkout2-head
assert_has_file firstfile
echo 'ok checkout firstfile'
assert_has_file baz/cow
assert_has_file baz/saucer
echo 'ok checkout baz (2)'
assert_has_file baz/deeper/ohyeah
assert_has_file baz/another/y
echo 'ok checkout verify exists'


