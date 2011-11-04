#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#
# Author: Colin Walters <walters@verbum.org>

set -e

echo "1..8"

. libtest.sh

setup_test_repository "regular"

$OSTREE checkout test2 checkout-test2

cd "${test_tmpdir}"
mkdir artifact-libfoo-runtime
cd artifact-libfoo-runtime
mkdir -p usr/lib/
echo 'an ELF file' > usr/lib/libfoo.so
mkdir -p usr/share
echo 'some data' > usr/share/foo.data

find | grep -v '^\.$' | $OSTREE commit -b artifact-libfoo-runtime -s 'Build 12345 of libfoo' --from-stdin

cd "${test_tmpdir}"
mkdir artifact-libfoo-devel
cd artifact-libfoo-devel
mkdir -p usr/include
echo 'a header' > usr/include/foo.h
mkdir -p usr/share/doc
echo 'some documentation' > usr/share/doc/foo.txt

find | grep -v '^\.$' | $OSTREE commit -b artifact-libfoo-devel -s 'Build 12345 of libfoo' --from-stdin

cd "${test_tmpdir}"
mkdir artifact-barapp
cd artifact-barapp
mkdir -p usr/bin
echo 'another ELF file' > usr/bin/bar

find | grep -v '^\.$' | $OSTREE commit -b artifact-barapp -s 'Build 42 of barapp' --from-stdin

echo 'ok artifacts committed'

cd "${test_tmpdir}"
$OSTREE compose some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
echo 'ok compose command'

cd some-compose
assert_file_has_content ./usr/bin/bar 'another ELF file'
assert_file_has_content ./usr/share/doc/foo.txt 'some documentation'
find | md5sum > ../some-compose-md5
assert_file_has_content ../some-compose-md5 9038703e43d2ff2745fb7dd844de65c8 

echo 'ok compose content'

cd "${test_tmpdir}"
rm -rf some-compose
$OSTREE compose --out-metadata=./some-compose-metadata some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
echo 'ok compose output metadata'

cd some-compose
$OSTREE commit --metadata-variant=${test_tmpdir}/some-compose-metadata -b some-compose -s 'Initial commit of some-compose'
echo 'ok compose commit with metadata'

$OSTREE show --print-compose some-compose > ${test_tmpdir}/some-compose-contents
assert_file_has_content ${test_tmpdir}/some-compose-contents artifact-libfoo-runtime
assert_file_has_content ${test_tmpdir}/some-compose-contents artifact-libfoo-devel
echo 'ok compose verify metadata'

cd "${test_tmpdir}"
rm -rf some-compose some-compose-metadata
cd "${test_tmpdir}"/artifact-barapp
echo 'updated bar ELF file' > usr/bin/bar
$OSTREE commit -b artifact-barapp -s 'Build 43 of barapp'

cd "${test_tmpdir}"
$OSTREE compose --out-metadata=./some-compose-metadata some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
cd some-compose
assert_file_has_content ./usr/bin/bar 'updated bar ELF file'

echo 'ok updated artifact barapp'
$OSTREE commit --metadata-variant=${test_tmpdir}/some-compose-metadata -b some-compose -s 'Updated some-compose'
cd ${test_tmpdir}
rm -rf some-compose

$OSTREE checkout some-compose some-compose-checkout
cd some-compose-checkout
assert_file_has_content ./usr/bin/bar 'updated bar ELF file'
echo 'ok updated compose commit'
