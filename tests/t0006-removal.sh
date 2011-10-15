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

echo '1..4'

setup_test_repository2
hacktree checkout $ht_repo HEAD $test_tmpdir/checkout2-head
echo 'ok setup'
cd $test_tmpdir/checkout2-head
hacktree commit -s delete $ht_repo -r firstfile
echo 'ok rm firstfile'
assert_has_file firstfile  # It should still exist in this checkout
cd $test_tmpdir
hacktree checkout $ht_repo HEAD $test_tmpdir/checkout3-head
echo 'ok checkout 3'
cd $test_tmpdir/checkout3-head
assert_not_has_file firstfile
assert_has_file baz/saucer
echo 'ok removal full'
