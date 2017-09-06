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

skip_without_fuse
skip_without_user_xattrs

setup_test_repository "bare"

echo "1..7"

cd ${test_tmpdir}
mkdir mnt
# The default content set amazingly doesn't have a non-broken link
ln -s firstfile files/firstfile-link
$OSTREE commit -b test2 --tree=dir=files
$OSTREE checkout -H test2 checkout-test2

rofiles-fuse checkout-test2 mnt
cleanup_fuse() {
    fusermount -u ${test_tmpdir}/mnt || true
}
trap cleanup_fuse EXIT
assert_file_has_content mnt/firstfile first
echo "ok mount"

# Test open(O_TRUNC) directly and via symlink
for path in firstfile{,-link}; do
    if cp /dev/null mnt/${path} 2>err.txt; then
        assert_not_reached "inplace mutation ${path}"
    fi
    assert_file_has_content err.txt "Read-only file system"
    assert_file_has_content mnt/firstfile first
    assert_file_has_content checkout-test2/firstfile first
done
echo "ok failed inplace mutation (open O_TRUNCATE)"

# Test chmod
if chmod 0600 mnt/firstfile 2>err.txt; then
    assert_not_reached "chmod inplace"
fi
assert_file_has_content err.txt "chmod:.*Read-only file system"
# Test chown with regfiles and symlinks
for path in firstfile baz/alink; do
    if chown -h $(id -u) mnt/${path} 2>err.txt; then
        assert_not_reached "chown inplace ${path}"
    fi
    assert_file_has_content err.txt "chown:.*Read-only file system"
done
# And test via dereferencing a symlink
if chown $(id -u) mnt/firstfile-link 2>err.txt; then
    assert_not_reached "chown inplace firstfile-link"
fi
assert_file_has_content err.txt "chown:.*Read-only file system"
echo "ok failed mutation chmod + chown"

# Test creating new files, using chown + chmod on them as well
echo anewfile-for-fuse > mnt/anewfile-for-fuse
assert_file_has_content mnt/anewfile-for-fuse anewfile-for-fuse
assert_file_has_content checkout-test2/anewfile-for-fuse anewfile-for-fuse
ln -s anewfile-for-fuse mnt/anewfile-for-fuse-link
# And also test modifications through a symlink
echo writevialink > mnt/anewfile-for-fuse-link
for path in anewfile-for-fuse{,-link}; do
    assert_file_has_content mnt/${path} writevialink
done
chown $(id -u) mnt/anewfile-for-fuse-link

mkdir mnt/newfusedir
for i in $(seq 5); do
    echo ${i}-morenewfuse-${i} > mnt/newfusedir/test-morenewfuse.${i}
    chmod 0600 mnt/newfusedir/test-morenewfuse.${i}
    chown $(id -u) mnt/newfusedir/test-morenewfuse.${i}
done
assert_file_has_content checkout-test2/newfusedir/test-morenewfuse.3 3-morenewfuse-3
echo "ok new content"

rm mnt/baz/cow
assert_not_has_file checkout-test2/baz/cow
rm mnt/baz/another -rf
assert_not_has_dir checkout-test2/baz/another

echo "ok deletion"

${CMD_PREFIX} ostree --repo=repo commit -b test2 -s fromfuse --link-checkout-speedup --tree=dir=checkout-test2

echo "ok commit"

${CMD_PREFIX} ostree --repo=repo checkout -U test2 mnt/test2-checkout-copy-fallback
assert_file_has_content mnt/test2-checkout-copy-fallback/anewfile-for-fuse writevialink

if ${CMD_PREFIX} ostree --repo=repo checkout -UH test2 mnt/test2-checkout-copy-hardlinked 2>err.txt; then
    assert_not_reached "Checking out via hardlinks across mountpoint unexpectedly succeeded!"
fi
assert_file_has_content err.txt "Unable to do hardlink checkout across devices"

echo "ok checkout copy fallback"
