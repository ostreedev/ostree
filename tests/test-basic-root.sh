#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+
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

id=$(id -u)

if test ${id} != 0; then
    skip "continued basic tests must be run as root (possibly in a container)"
fi

setup_test_repository "bare"

echo "1..1"

nextid=$(($id + 1))

rm checkout-test2 -rf
$OSTREE checkout test2 checkout-test2
$OSTREE commit ${COMMIT_ARGS} -b test2 --tree=ref=test2 --owner-uid=$nextid
$OSTREE ls test2 baz/cow > ls.txt
assert_file_has_content ls.txt '-00644 '${nextid}' '${id}
# As bare and running as root (e.g. Docker container), do some ownership tests
# https://github.com/ostreedev/ostree/pull/801
# Both hardlinks and copies should respect ownership, but we don't have -C yet;
# add it when we do.
for opt in -H; do
    rm test2-co -rf
    $OSTREE checkout ${opt} test2 test2-co
    assert_streq "$(stat -c '%u' test2-co/baz/cow)" ${nextid}
    assert_streq "$(stat -c '%u' test2-co/baz/alink)" ${nextid}
done
rm test2-co -rf
# But user mode doesn't
$OSTREE checkout -U test2 test2-co
assert_streq "$(stat -c '%u' test2-co/baz/cow)" ${id}
assert_streq "$(stat -c '%u' test2-co/baz/alink)" ${id}
echo "ok ownership"
