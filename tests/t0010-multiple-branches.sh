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

echo '1..3'

setup_test_repository2
ostree checkout $ot_repo test2 $test_tmpdir/checkout2-head
cd $test_tmpdir/checkout2-head
echo test3file > test3file
ostree commit $ot_repo -s 'Add test3file' -b test3 -p test2 --add test3file
echo "ok commit test3 branch"
ostree checkout $ot_repo test3 $test_tmpdir/checkout3-head
echo "ok checkout test3 branch"
cd $test_tmpdir/checkout3-head
assert_has_file test3file
echo "ok checkout test3file"
