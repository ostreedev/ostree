#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

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
