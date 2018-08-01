#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
# Copyright (C) 2015 Red Hat, Inc.
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

echo "1..4"

. $(dirname $0)/libtest.sh

setup_test_repository "bare"
echo "ok setup"

# Check that without commit-update-summary set, creating a commit doesn't update the summary
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

# Check that with commit-update-summary set, creating a commit updates the summary
$OSTREE --repo=repo config set core.commit-update-summary true

echo hello3 > test/a

$OSTREE commit -b test -s "Another commit..." test
echo "ok commit 3"

assert_not_streq "$OLD_MD5" "$(md5sum repo/summary)"

$OSTREE --repo=repo config set core.commit-update-summary false

# Check that summary --update deletes the .sig file
touch repo/summary.sig
$OSTREE summary --update
assert_not_has_file repo/summary.sig

# Check that without auto-update-summary set, adding, changing, or deleting a ref doesn't update the summary
$OSTREE summary --update
OLD_MD5=$(md5sum repo/summary)
$OSTREE commit -b test2 -s "A commit" test

assert_streq "$OLD_MD5" "$(md5sum repo/summary)"

$OSTREE reset test test^

assert_streq "$OLD_MD5" "$(md5sum repo/summary)"

$OSTREE refs --delete test

assert_streq "$OLD_MD5" "$(md5sum repo/summary)"

# Check that with auto-update-summary set, adding, changing, or deleting a ref updates the summary
$OSTREE --repo=repo config set core.auto-update-summary true

$OSTREE summary --update
OLD_MD5=$(md5sum repo/summary)
echo hello > test/a
$OSTREE commit -b test -s "A commit" test
echo hello2 > test/a
$OSTREE commit -b test -s "Another commit" test

assert_not_streq "$OLD_MD5" "$(md5sum repo/summary)"

OLD_MD5=$(md5sum repo/summary)
$OSTREE reset test test^

assert_not_streq "$OLD_MD5" "$(md5sum repo/summary)"

OLD_MD5=$(md5sum repo/summary)
$OSTREE refs --delete test

assert_not_streq "$OLD_MD5" "$(md5sum repo/summary)"
