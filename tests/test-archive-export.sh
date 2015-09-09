#!/bin/bash
#
# Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

echo "1..2"

. $(dirname $0)/libtest.sh

setup_test_repository "archive-z2"
rev=$(ostree --repo=repo rev-parse test2)
ostree --repo=repo archive-export test2 > test2.tar
mkdir tmp
(cd tmp && tar xf ../test2.tar)
assert_has_file tmp/${rev}.commit
rm tmp -rf

echo "ok export"

mkdir repo2 && ostree --repo=repo2 init --mode=archive-z2
ostree --repo=repo2 archive-import test2 < test2.tar
rev2=$(ostree --repo=repo2 rev-parse test2)
if test "${rev}" != "${rev2}"; then
    assert_not_reached
fi
ostree --repo=repo2 checkout test2 test2-checkout
assert_file_has_content test2-checkout/baz/another/y '^x$'
rm test2-checkout -rf

mkdir repo3 && ostree --repo=repo3 init --mode=bare-user
ostree --repo=repo3 archive-import test2 < test2.tar
rev2=$(ostree --repo=repo3 rev-parse test2)
if test "${rev}" != "${rev2}"; then
    assert_not_reached
fi
ostree --repo=repo3 checkout test2 test2-checkout
assert_file_has_content test2-checkout/baz/another/y '^x$'
rm test2-checkout -rf

echo "ok import"
