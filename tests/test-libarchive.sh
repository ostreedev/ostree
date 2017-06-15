#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

echo "1..21"

setup_test_repository "bare"

cd ${test_tmpdir}
mkdir foo
cd foo
mkdir -p usr/bin
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

tar -c -z -f ../foo.tar.gz .
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
  cd ${test_tmpdir}
  $OSTREE checkout test-$1 test-$1-checkout
  cd test-$1-checkout

  # basic content check
  assert_file_has_content usr/bin/foo contents
  assert_file_has_content usr/bin/bar contents
  assert_file_has_content usr/local/bin/baz contents
  assert_file_empty usr/bin/foo0
  assert_file_empty usr/bin/bar0
  assert_file_empty usr/local/bin/baz0
  echo "ok $1 contents"

  # hardlinks
  assert_files_hardlinked usr/bin/foo usr/bin/bar
  assert_files_hardlinked usr/bin/foo usr/local/bin/baz
  echo "ok $1 hardlink"
  assert_files_hardlinked usr/bin/foo0 usr/bin/bar0
  assert_files_hardlinked usr/bin/foo0 usr/local/bin/baz0
  echo "ok $1 hardlink to empty files"

  # symlinks
  assert_symlink_has_content usr/bin/sl foo
  assert_file_has_content usr/bin/sl contents
  echo "ok $1 symlink"
  # ostree checkout doesn't care if two symlinks are actually hardlinked
  # together (which is fine). checking that it's also a symlink is good enough.
  assert_symlink_has_content usr/local/bin/slhl foo
  echo "ok $1 hardlink to symlink"

  # stat override
  test -u usr/bin/setuidme
  echo "ok $1 setuid"

  # skip list
  test ! -f usr/bin/skipme
  echo "ok $1 file skip"

  cd ${test_tmpdir}
  rm -rf test-$1-checkout
}

assert_valid_checkout tar
assert_valid_checkout cpio

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


cd ${test_tmpdir}
# We need a bare-user repo for this
rm repo -rf
$OSTREE --repo=repo init --mode=bare-user
# Yeah this is ugly but it's simpler than adding it to the installed tests etc.
# We need a tarball that has a device file.
python -c 'import base64,sys; base64.decode(sys.stdin, sys.stdout)' > testlayer.tar.gz <<EOF
H4sIAMKpqFcAA+2WS07DMBCGveYUPkEcP2eNRJdsuEEpWSBKjZymErfHjpGCsmioFLs8/m8zeUkz0TdjuxGsOG2ErB1jZB7Ha6nIKqmUkTI+J2od47Z8aYwN/XEbOGfB++O575be/1Ia8dSdCvfAJf5t8i9b4yz81yD7Pwz7fbkcSbBzbtF/bBLtKPaJVM7E+dflSppYy3/+D/kZq5S+Btn/zoeuXI5x/om+6T/NvzbKMK7EW/A78VK0Osx/9P+wub2735TKMc6/MWf802z9Tw3AeFuqoK/8c//Z/M21ywBXohFDH37Q+c/k85+VhPNfDbL/x+dDwR64zD+l9T/ewX8NJv+9f+1i3Ib3tXMs7v9Sz/xr7bD/V2GyjjMAAAAAAAAAAAAAAAAA/CU+AAWVA4wAKAAA
EOF
gunzip testlayer.tar.gz
# Now let's extend it here in a more readable fashion.  This tests unwritable directories
mkdir -p testlayer.new/usr/notwritable
echo somebinary > testlayer.new/usr/notwritable/somebinary
chmod a-w testlayer.new/usr/notwritable
echo worldwritable > testlayer.new/usr/worldwritablefile
chmod a+w testlayer.new/usr/worldwritablefile
tar -r -f testlayer.tar -C testlayer.new .
gzip testlayer.tar
$OSTREE commit -b ociimage --tree=oci-layer=testlayer.tar.gz
chmod -R u+w testlayer.new && rm testlayer.new -rf
$OSTREE checkout -U ociimage ociimage
assert_file_has_content ociimage/usr/bin/somebinary somebinary
assert_not_has_file ociimage/dev/null
test -L ociimage/dev/core
test -w ociimage/usr/notwritable
assert_has_file ociimage/dev/README
rm -rf testlayer.tar.gz ociimage
echo "ok ociimage"
