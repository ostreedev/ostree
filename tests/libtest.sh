# Source library for shell script tests
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

TMPDIR=${TMPDIR:-/tmp}
export TMPDIR
test_tmpdir=`mktemp -d "$TMPDIR/hacktree-tests.XXXXXXXXXX"`
cd "$test_tmpdir"
touch "$test_tmpdir/.test$$"

die () {
    test -f "$test_tmpdir/.test$$" && rm -rf "$test_tmpdir"
}

trap 'die' EXIT
