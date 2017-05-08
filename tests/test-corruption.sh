#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

echo "1..3"

. $(dirname $0)/libtest.sh

cd ${test_tmpdir}
rm repo files -rf
setup_test_repository "bare"
$OSTREE checkout test2 checkout-test2
cd checkout-test2
chmod o+x firstfile
$OSTREE fsck -q && (echo 1>&2 "fsck unexpectedly succeeded"; exit 1)
chmod o-x firstfile
$OSTREE fsck -q

echo "ok chmod"

cd ${test_tmpdir}
rm repo files -rf
setup_test_repository "bare"
rm checkout-test2 -rf
$OSTREE checkout test2 checkout-test2
cd checkout-test2
chmod o+x firstfile
$OSTREE fsck -q --delete && (echo 1>&2 "fsck unexpectedly succeeded"; exit 1)

echo "ok chmod"

cd ${test_tmpdir}
rm repo files -rf
setup_test_repository "bare"
find repo/ -name '*.commit' -delete
if $OSTREE fsck -q 2>err.txt; then
    assert_not_reached "fsck unexpectedly succeeded"
fi
assert_file_has_content_literal err.txt "Loading commit for ref test2: No such metadata object"

echo "ok missing commit"
