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

ostree checkout $ot_repo test2 checkout-test2

cd "${test_tmpdir}"
mkdir artifact-libfoo-runtime
cd artifact-libfoo-runtime
mkdir -p usr/lib/
echo 'an ELF file' > usr/lib/libfoo.so
mkdir -p usr/share
echo 'some data' > usr/share/foo.data

find | grep -v '^\.$' | ostree commit $ot_repo -b artifact-libfoo-runtime -s 'Build 12345 of libfoo' --from-stdin

cd "${test_tmpdir}"
mkdir artifact-libfoo-devel
cd artifact-libfoo-devel
mkdir -p usr/include
echo 'a header' > usr/include/foo.h
mkdir -p usr/share/doc
echo 'some documentation' > usr/share/doc/foo.txt

find | grep -v '^\.$' | ostree commit $ot_repo -b artifact-libfoo-devel -s 'Build 12345 of libfoo' --from-stdin

cd "${test_tmpdir}"
mkdir artifact-barapp
cd artifact-barapp
mkdir -p usr/bin
echo 'another ELF file' > usr/bin/bar

find | grep -v '^\.$' | ostree commit $ot_repo -b artifact-barapp -s 'Build 42 of barapp' --from-stdin

echo 'ok artifacts committed'

cd "${test_tmpdir}"
ostree checkout $ot_repo --compose some-compose artifact-libfoo-runtime artifact-libfoo-devel artifact-barapp
echo 'ok compose command'
