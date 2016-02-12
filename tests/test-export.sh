#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

. $(dirname $0)/libtest.sh

setup_test_repository "archive-z2"

echo '1..2'

$OSTREE checkout test2 test2-co
$OSTREE commit --no-xattrs -b test2-noxattrs -s "test2 without xattrs" --tree=dir=test2-co
rm test2-co -rf

cd ${test_tmpdir}
${OSTREE} 'export' test2-noxattrs -o test2.tar
mkdir t
(cd t && tar xf ../test2.tar)
ostree --repo=repo diff --no-xattrs test2-noxattrs ./t > diff.txt
assert_file_empty diff.txt
rm test2.tar diff.txt t -rf

echo 'ok export gnutar diff (no xattrs)'

cd ${test_tmpdir}
${OSTREE} 'export' test2 -o test2.tar
${OSTREE} commit -b test2-from-tar -s 'Import from tar' --tree=tar=test2.tar
ostree --repo=repo diff test2 test2-from-tar
assert_file_empty diff.txt
rm test2.tar diff.txt t -rf

echo 'ok export import'

