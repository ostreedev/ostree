#!/bin/bash
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

echo "1..1"

. $(dirname $0)/libtest.sh

setup_test_repository "bare"

cd ${test_tmpdir}
cat > test-statoverride.txt <<EOF
+2048 /abinary
EOF
$OSTREE checkout test2 test2-checkout
touch test2-checkout/abinary
chmod a+x test2-checkout/abinary
(cd test2-checkout && $OSTREE commit -b test2 -s "with statoverride" --statoverride=../test-statoverride.txt)
rm -rf test2-checkout
$OSTREE checkout test2 test2-checkout
test -u test2-checkout/abinary
