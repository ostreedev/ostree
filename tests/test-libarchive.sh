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

if ! ostree --version | grep -q -e '- libarchive'; then
    echo "1..0 #SKIP no libarchive support compiled in"
    exit 0
fi

. $(dirname $0)/libtest.sh

echo "1..13"

setup_test_repository "bare"

cd ${test_tmpdir}
mkdir foo
cd foo
mkdir -p usr/bin usr/lib
echo contents > usr/bin/foo
touch usr/bin/foo0
ln usr/bin/foo usr/bin/bar
ln usr/bin/foo0 usr/bin/bar0
ln -s foo usr/bin/sl
mkdir -p usr/local/bin
ln usr/bin/foo usr/local/bin/baz
ln usr/bin/foo0 usr/local/bin/baz0
ln usr/bin/sl usr/local/bin/slhl
touch usr/bin/setuidme
touch usr/bin/skipme
echo "a library" > usr/lib/libfoo.so
echo "another library" > usr/lib/libbar.so

# Create a tar archive
tar -c -z -f ../foo.tar.gz .
# Create a cpio archive
find . | cpio -o -H newc > ../foo.cpio

cd ..

cat > statoverride.txt <<EOF
2048 /usr/bin/setuidme
EOF

cat > skiplist.txt <<EOF
/usr/bin/skipme
EOF

$OSTREE commit -s "from tar" -b test-tar \
  --statoverride=statoverride.txt \
  --skip-list=skiplist.txt \
  --tree=tar=foo.tar.gz
echo "ok tar commit"
$OSTREE commit -s "from cpio" -b test-cpio \
  --statoverride=statoverride.txt \
  --skip-list=skiplist.txt \
  --tree=tar=foo.cpio
echo "ok cpio commit"

assert_valid_checkout () {
  ref=$1
  rm test-${ref}-checkout -rf
  $OSTREE checkout test-${ref} test-${ref}-checkout

  assert_valid_content test-${ref}-checkout
  rm -rf test-${ref}-checkout
}

assert_valid_content () {
  dn=$1
  cd ${dn}
  # basic content check
  assert_file_has_content usr/bin/foo contents
  assert_file_has_content usr/bin/bar contents
  assert_file_has_content usr/local/bin/baz contents
  assert_file_empty usr/bin/foo0
  assert_file_empty usr/bin/bar0
  assert_file_empty usr/local/bin/baz0
  assert_file_has_content usr/lib/libfoo.so 'a library'
  assert_file_has_content usr/lib/libbar.so 'another library'

  # hardlinks
  assert_files_hardlinked usr/bin/foo usr/bin/bar
  assert_files_hardlinked usr/bin/foo usr/local/bin/baz
  assert_files_hardlinked usr/bin/foo0 usr/bin/bar0
  assert_files_hardlinked usr/bin/foo0 usr/local/bin/baz0

  # symlinks
  assert_symlink_has_content usr/bin/sl foo
  assert_file_has_content usr/bin/sl contents
  # ostree checkout doesn't care if two symlinks are actually hardlinked
  # together (which is fine). checking that it's also a symlink is good enough.
  assert_symlink_has_content usr/local/bin/slhl foo

  # stat override
  test -u usr/bin/setuidme

  # skip list
  test ! -f usr/bin/skipme

  cd ${test_tmpdir}
}

assert_valid_checkout tar
echo "ok tar contents"
assert_valid_checkout cpio
echo "ok cpio contents"

cd ${test_tmpdir}
mkdir multicommit-files
cd multicommit-files
mkdir -p files1/subdir files2/subdir
echo "to be overwritten file" > files1/testfile
echo "not overwritten" > files1/otherfile
echo "overwriting file" > files2/testfile
ln -s "to-be-overwritten-symlink" files1/testsymlink
ln -s "overwriting-symlink" files2/testsymlink
ln -s "not overwriting symlink" files2/ohersymlink
echo "original" > files1/subdir/original
echo "new" > files2/subdir/new

tar -c -C files1 -z -f files1.tar.gz .
tar -c -C files2 -z -f files2.tar.gz .

$OSTREE commit -s 'multi tar' -b multicommit --tree=tar=files1.tar.gz --tree=tar=files2.tar.gz
echo "ok tar multicommit"

cd ${test_tmpdir}
$OSTREE checkout multicommit multicommit-checkout
cd multicommit-checkout
assert_file_has_content testfile "overwriting file"
assert_file_has_content otherfile "not overwritten"
assert_file_has_content subdir/original "original"
assert_file_has_content subdir/new "new"
echo "ok tar multicommit contents"

cd ${test_tmpdir}/multicommit-files
tar -c -C files1 -z -f partial.tar.gz subdir/original
$OSTREE commit -s 'partial' -b partial --tar-autocreate-parents --tree=tar=partial.tar.gz
echo "ok tar partial commit"

cd ${test_tmpdir}
$OSTREE checkout partial partial-checkout
cd partial-checkout
assert_file_has_content subdir/original "original"
echo "ok tar partial commit contents"

uid=$(id -u)
gid=$(id -g)
autocreate_args="--tar-autocreate-parents --owner-uid=${uid} --owner-gid=${gid}"

cd ${test_tmpdir}
tar -cf empty.tar.gz -T /dev/null
$OSTREE commit -b tar-empty ${autocreate_args} --tree=tar=empty.tar.gz
$OSTREE ls tar-empty > ls.txt
assert_file_has_content ls.txt "d00755 ${uid} ${gid}      0 /"
echo "ok tar autocreate with owner uid/gid"

# noop pathname filter
cd ${test_tmpdir}
$OSTREE commit -b test-tar ${autocreate_args} \
        --tar-pathname-filter='^nosuchfile/,nootherfile/' \
        --statoverride=statoverride.txt \
        --skip-list=skiplist.txt \
        --tree=tar=foo.tar.gz
rm test-tar-co -rf
$OSTREE checkout test-tar test-tar-co
assert_valid_content ${test_tmpdir}/test-tar-co
echo "ok tar pathname filter prefix (noop)"

# Add a prefix
cd ${test_tmpdir}
# Update the metadata overrides matching our pathname filter
for f in statoverride.txt skiplist.txt; do
    sed -i -e 's,/usr/,/foo/usr/,' $f
done
$OSTREE commit -b test-tar ${autocreate_args} \
        --tar-pathname-filter='^,foo/' \
        --statoverride=statoverride.txt \
        --skip-list=skiplist.txt \
        --tree=tar=foo.tar.gz
rm test-tar-co -rf
$OSTREE checkout test-tar test-tar-co
assert_has_dir test-tar-co/foo
assert_valid_content ${test_tmpdir}/test-tar-co/foo
echo "ok tar pathname filter prefix"

# Test anchored and not-anchored
for filter in '^usr/bin/,usr/sbin/' '/bin/,/sbin/'; do
    cd ${test_tmpdir}
    $OSTREE commit -b test-tar ${autocreate_args} \
            --tar-pathname-filter=$filter \
            --tree=tar=foo.tar.gz
    rm test-tar-co -rf
    $OSTREE checkout test-tar test-tar-co
    cd test-tar-co
    # Check that we just had usr/bin → usr/sbin
    assert_not_has_file usr/bin/foo
    assert_file_has_content usr/sbin/foo contents
    assert_not_has_file usr/sbin/libfoo.so
    assert_file_has_content usr/lib/libfoo.so 'a library'
    echo "ok tar pathname filter modification: ${filter}"
done
