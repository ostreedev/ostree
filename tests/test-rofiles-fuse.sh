#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

skip_without_fuse
skip_without_user_xattrs

user=$(env | grep USER | cut -d "=" -f 2)
if [ "$user" != "root" ]
then
	skip "user:$user does not support running the test case"
fi

setup_test_repository "bare"

echo "1..13"

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
libtest_exit_cmds+=(cleanup_fuse)
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

if setfattr -n user.foo -v bar mnt/firstfile-link; then
    assert_not_reached "set xattr on linked file"
fi

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

setfattr -n user.foo -v bar mnt/anewfile-for-fuse
getfattr -d -m . mnt/anewfile-for-fuse > out.txt
assert_file_has_content_literal out.txt 'user.foo="bar"'
echo "ok new xattrs"

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

# check that O_RDONLY|O_CREAT is handled correctly; used by flock(1) at least
flock mnt/nonexistent-file echo "ok create file in ro mode"
echo "ok flock"

# And now with --copyup enabled

copyup_reset() {
    cd ${test_tmpdir}
    fusermount -u mnt
    rm checkout-test2 -rf
    $OSTREE checkout -H test2 checkout-test2
    rofiles-fuse --copyup checkout-test2 mnt
}

assert_test_file() {
    t=$1
    f=$2
    if ! test ${t} "${f}"; then
        ls -al "${f}"
        fatal "Failed test ${t} ${f}"
    fi
}

copyup_reset
assert_file_has_content mnt/firstfile first
echo "ok copyup mount"

# Test O_TRUNC directly
firstfile_orig_inode=$(stat -c %i checkout-test2/firstfile)
echo -n truncating > mnt/firstfile
assert_streq "$(cat mnt/firstfile)" truncating
firstfile_new_inode=$(stat -c %i checkout-test2/firstfile)
assert_not_streq "${firstfile_orig_inode}" "${firstfile_new_inode}"
assert_test_file -f checkout-test2/firstfile

# Test xattr modifications
copyup_reset
firstfile_orig_inode=$(stat -c %i checkout-test2/firstfile)
setfattr -n user.foo -v bar mnt/firstfile
getfattr -d -m . mnt/firstfile > out.txt
assert_file_has_content_literal out.txt 'user.foo="bar"'
firstfile_new_inode=$(stat -c %i checkout-test2/firstfile)
assert_not_streq "${firstfile_orig_inode}" "${firstfile_new_inode}"
assert_test_file -f checkout-test2/firstfile

copyup_reset
firstfile_link_orig_inode=$(stat -c %i checkout-test2/firstfile-link)
firstfile_orig_inode=$(stat -c %i checkout-test2/firstfile)
# Now write via the symlink
echo -n truncating > mnt/firstfile-link
assert_streq "$(cat mnt/firstfile)" truncating
firstfile_new_inode=$(stat -c %i checkout-test2/firstfile)
firstfile_link_new_inode=$(stat -c %i checkout-test2/firstfile-link)
assert_not_streq "${firstfile_orig_inode}" "${firstfile_new_inode}"
assert_streq "${firstfile_link_orig_inode}" "${firstfile_link_new_inode}"
assert_test_file -f checkout-test2/firstfile
# Verify we didn't replace the link with a regfile somehow
assert_test_file -L checkout-test2/firstfile-link

# These both end up creating new files; in the sed case we'll then do a rename()
copyup_reset
echo "hello new file" > mnt/a-new-non-copyup-file
assert_file_has_content_literal mnt/a-new-non-copyup-file "hello new file"
sed -i -e s,first,second, mnt/firstfile
assert_file_has_content_literal mnt/firstfile "second"

echo "ok copyup"

copyup_reset
echo nonhardlinked > checkout-test2/nonhardlinked
if fsverity enable checkout-test2/nonhardlinked 2>err.txt; then
    orig_inode=$(stat -c %i checkout-test2/nonhardlinked)
    echo "updated content" > mnt/nonhardlinked
    new_inode=$(stat -c %i checkout-test2/nonhardlinked)
    assert_not_streq "${orig_inode}" "${new_inode}"
    # And via chmod
    fsverity enable checkout-test2/nonhardlinked
    orig_inode=$(stat -c %i checkout-test2/nonhardlinked)
    chmod 0700 mnt/nonhardlinked
    new_inode=$(stat -c %i checkout-test2/nonhardlinked)
    assert_not_streq "${orig_inode}" "${new_inode}"
    echo "ok copyup fsverity"
else
    echo "ok copyup fsverity # SKIP no fsverity support: $(cat err.txt)"
fi
