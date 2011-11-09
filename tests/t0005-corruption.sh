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

echo "1..1"

. libtest.sh

setup_test_repository "regular"
$OSTREE checkout test2 checkout-test2
cd checkout-test2
chmod o+x firstfile
$OSTREE fsck -q 2>/dev/null && (echo 1>&2 "fsck unexpectedly succeeded"; exit 1)
chmod o-x firstfile
$OSTREE fsck -q

echo "ok chmod"
