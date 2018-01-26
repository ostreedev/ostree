#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
# Copyright (C) 2015 Red Hat, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

echo "1..4"

. $(dirname $0)/libtest.sh

setup_test_repository "bare"
echo "ok setup"

mkdir test

echo hello > test/a

$OSTREE commit -b test -s "A commit" test
echo "ok commit 1"

$OSTREE summary --update

OLD_MD5=$(md5sum repo/summary)

echo hello2 > test/a

$OSTREE commit -b test -s "Another commit" test
echo "ok commit 2"

assert_streq "$OLD_MD5" "$(md5sum repo/summary)"

$OSTREE --repo=repo config set core.commit-update-summary true

echo hello3 > test/a

$OSTREE commit -b test -s "Another commit..." test
echo "ok commit 3"

assert_not_streq "$OLD_MD5" "$(md5sum repo/summary)"

# Check that summary --update deletes the .sig file
touch repo/summary.sig
$OSTREE summary --update
assert_not_has_file repo/summary.sig
