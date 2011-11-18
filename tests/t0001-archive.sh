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

set -e

. libtest.sh

echo '1..5'

setup_test_repository "archive"
echo "ok setup"

$OSTREE checkout test2 checkout-test2
echo "ok checkout"

cd checkout-test2
assert_has_file firstfile
assert_has_file baz/cow
assert_file_has_content baz/cow moo
assert_has_file baz/deeper/ohyeah
echo "ok content"

cd ${test_tmpdir}
mkdir repo2
ostree --repo=repo2 init
$OSTREE local-clone repo2
echo "ok local clone"

cd ${test_tmpdir}
ostree --repo=repo2 checkout test2 test2-checkout-from-local-clone
cd test2-checkout-from-local-clone
assert_file_has_content baz/cow moo
echo "ok local clone checkout"
