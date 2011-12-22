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

set -e

echo "1..6"

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

$OSTREE commit -b artifact-libfoo-runtime -s 'Build 12345 of libfoo'

cd "${test_tmpdir}"
mkdir artifact-libfoo-devel
cd artifact-libfoo-devel
mkdir -p usr/include
echo 'a header' > usr/include/foo.h
mkdir -p usr/share/doc
echo 'some documentation' > usr/share/doc/foo.txt

$OSTREE commit -b artifact-libfoo-devel -s 'Build 12345 of libfoo'

cd "${test_tmpdir}"
mkdir artifact-barapp
cd artifact-barapp
mkdir -p usr/bin
echo 'another ELF file' > usr/bin/bar

$OSTREE commit -b artifact-barapp -s 'Build 42 of barapp'

echo 'ok artifacts committed'

cd "${test_tmpdir}"
$OSTREE compose -s "compose 1" -b some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
echo 'ok compose'

$OSTREE checkout some-compose some-compose-checkout
cd some-compose-checkout
assert_file_has_content ./usr/bin/bar 'another ELF file'
assert_file_has_content ./usr/share/doc/foo.txt 'some documentation'
find | md5sum > ../some-compose-md5
assert_file_has_content ../some-compose-md5 9038703e43d2ff2745fb7dd844de65c8 

echo 'ok compose content'

cd "${test_tmpdir}"
rm -rf some-compose-checkout some-compose-metadata
cd "${test_tmpdir}"/artifact-barapp
echo 'updated bar ELF file' > usr/bin/bar
$OSTREE commit -b artifact-barapp -s 'Build 43 of barapp'
$OSTREE compose -s "compose 2" -b some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
echo 'ok compose update commit'

cd "${test_tmpdir}"
$OSTREE checkout some-compose some-compose-checkout
cd some-compose-checkout
assert_file_has_content ./usr/bin/bar 'updated bar ELF file'
echo 'ok compose update contents'

cd "${test_tmpdir}"
$OSTREE compose --recompose -b some-compose -s 'Recompose'
rm -rf some-compose-checkout
$OSTREE checkout some-compose some-compose-checkout
cd some-compose-checkout
assert_file_has_content ./usr/bin/bar 'updated bar ELF file'
echo 'ok recompose'
