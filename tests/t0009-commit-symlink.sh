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

ostree checkout $ot_repo HEAD $test_tmpdir/checkout2-head
cd $ht_files
ln -s foo bar
ostree commit $ot_repo -s "Add a symlink" -b "To test it" --add=bar
echo "ok commit symlink"
ostree fsck $ot_repo
echo "ok fsck"
