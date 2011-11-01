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

mkdir files
cd files
echo first > firstfile
echo second > secondfile
ln -s foo bar

mkdir ../repo
repo="--repo=../repo"
ostree init --archive $repo
echo 'ok init'
ostree commit $repo -b test -s "Test Commit 1" -m "Commit body first" --add=firstfile
echo 'ok commit 1'
ostree commit $repo -b test -s "Test Commit 2" -m "Commit body first" --add=secondfile --add=bar
echo 'ok commit 2'
ostree fsck -q $repo
echo 'ok fsck'
