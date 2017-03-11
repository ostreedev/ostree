#!/bin/bash
#
# Copyright (C) 2016 Red Hat, Inc.
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

skip_without_fuse
skip_without_user_xattrs

setup_test_repository "bare"

if ! touch .wh.whiteout; then
    skip "Cannot use whiteout files"
fi

echo "1..6"

mkdir -p tree-1/sub
touch tree-1/{1,2,3,will_be_removed}
ln -s /2 tree-1/link
echo wrong > tree-1/sub/removed_and_readded
echo content_for_file_3 > tree-1/sub/3
setfattr -n user.foo -v bar tree-1/1

mkdir -p tree-2/sub
touch tree-2/{a,b,c,.wh.will_be_removed,sub/.wh.removed_and_readded}

mkdir -p tree-3/sub
touch tree-3/sub/removed_and_readded
echo correct > tree-3/sub/removed_and_readded

for i in $(seq 3); do
    ${CMD_PREFIX} ostree --repo=repo commit -b tree-$i tree-$i
done

# So to avoid completely the risk of using the wrong files for tests.
rm -rf tree-1 tree-2 tree-3

mkdir mnt

ostree-union-fuse -o whiteouts -o repo=repo -o layers=tree-1:tree-2:tree-3 mnt

cleanup_fuse() {
    fusermount -u ${test_tmpdir}/mnt || true
}
trap cleanup_fuse EXIT


cat mnt/1

echo "ok mount"

if cp /dev/null mnt 2>err.txt; then
    assert_not_reached "write operation successful"
fi

if rm mnt/1 2>err.txt; then
    assert_not_reached "rm operation successful"
fi

echo "ok write not allowed"

readlink mnt/link > readlink.out
assert_file_has_content readlink.out "/2"

echo "ok symlink"

getfattr -d mnt/1 > attr
assert_file_has_content attr "foo"
assert_file_has_content attr "bar"

echo "ok xattr"

assert_file_has_content mnt/sub/3 content_for_file_3

echo "ok read"

assert_not_has_file "mnt/will_be_removed"
assert_file_has_content "mnt/sub/removed_and_readded" "correct"

echo "ok whiteout delete"
