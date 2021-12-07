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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..16'

setup_test_repository "bare"
$OSTREE remote add origin http://example.com/ostree/gnome
$OSTREE remote show-url origin >/dev/null
echo "ok config"

$OSTREE remote add --no-sign-verify another http://another.com/repo
$OSTREE remote show-url another >/dev/null
echo "ok remote no gpg-verify"

if $OSTREE remote add --no-sign-verify another http://another.example.com/anotherrepo 2>err.txt; then
    assert_not_reached "Adding duplicate remote unexpectedly succeeded"
fi
echo "ok"

$OSTREE remote add --if-not-exists --no-sign-verify another http://another.example.com/anotherrepo
$OSTREE remote show-url another >/dev/null
echo "ok"

$OSTREE remote add --if-not-exists --no-sign-verify another-noexist http://another-noexist.example.com/anotherrepo
$OSTREE remote show-url another-noexist >/dev/null
echo "ok"

$OSTREE remote list > list.txt
assert_file_has_content list.txt "origin"
assert_file_has_content list.txt "another"
assert_file_has_content list.txt "another-noexist"
assert_not_file_has_content list.txt "http://example\.com/ostree/gnome"
assert_not_file_has_content list.txt "http://another\.com/repo"
assert_not_file_has_content list.txt "http://another-noexist\.example\.com/anotherrepo"
echo "ok remote list"

$OSTREE remote list --show-urls > list.txt
assert_file_has_content list.txt "origin"
assert_file_has_content list.txt "another"
assert_file_has_content list.txt "another-noexist"
assert_file_has_content list.txt "http://example\.com/ostree/gnome"
assert_file_has_content list.txt "http://another\.com/repo"
assert_file_has_content list.txt "http://another-noexist\.example\.com/anotherrepo"
echo "ok remote list with urls"

cd ${test_tmpdir}
rm -rf parent-repo
ostree_repo_init parent-repo
$OSTREE config set core.parent ${test_tmpdir}/parent-repo
${CMD_PREFIX} ostree --repo=parent-repo remote add --no-sign-verify parent-remote http://parent-remote.example.com/parent-remote
$OSTREE remote list > list.txt
assert_file_has_content list.txt "origin"
assert_file_has_content list.txt "another"
assert_file_has_content list.txt "another-noexist"
assert_file_has_content list.txt "parent-remote"
echo "ok remote list with parent repo remotes"

$OSTREE remote delete another
echo "ok remote delete"

if $OSTREE remote delete nosuchremote 2>err.txt; then
    assert_not_reached "Deleting remote unexpectedly succeeded"
fi
assert_file_has_content err.txt "error: "

$OSTREE remote delete --if-exists nosuchremote
echo "ok"

if $OSTREE remote show-url nosuchremote 2>/dev/null; then
    assert_not_reached "Deleting remote unexpectedly failed"
fi
echo "ok"

$OSTREE remote delete --if-exists origin
echo "ok"

if $OSTREE remote show-url origin 2>/dev/null; then
    assert_not_reached "Deleting remote unexpectedly failed"
fi
echo "ok"

$OSTREE remote list > list.txt
assert_not_file_has_content list.txt "origin"
# Can't grep for 'another' because of 'another-noexist'
assert_file_has_content list.txt "another-noexist"
echo "ok remote list remaining"

# Both --if-not-exists and --force cannot be used
if $OSTREE remote add --if-not-exists --force yetanother http://yetanother.com/repo 2>/dev/null; then
    assert_not_reached "Adding remote with --if-not-exists and --force unexpectedly succeeded"
fi
echo "ok remote add fail --if-not-exists and --force"

# Overwrite with --force
$OSTREE remote add --force another http://another.example.com/anotherrepo
$OSTREE remote list --show-urls > list.txt
assert_file_has_content list.txt "^another \+http://another\.example\.com/anotherrepo$"
echo "ok remote add --force"
