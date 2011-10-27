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

echo "1..2"

setup_test_repository2
ostree checkout $ot_repo master $test_tmpdir/checkout2-head
cd $test_tmpdir/checkout2-head
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
find | grep -v '^\.$' | ostree commit $ot_repo --from-stdin -s "From find"
echo "ok commit stdin"
ostree checkout $ot_repo master $test_tmpdir/checkout3-head
cd $test_tmpdir/checkout3-head
assert_has_file a/nested/2
assert_file_has_content a/nested/2 'two2'
echo "ok stdin contents"
