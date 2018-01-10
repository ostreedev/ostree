#!/bin/bash

# Copyright (C) 2018 William Manley <will@williammanley.net>
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

echo "1..2"

#### Test setup: Create some interesting trees

setup_test_repository bare

mkdir pristine
cd pristine

mkdir a
echo basic >a/file

mkdir b
echo basic >b/file
echo repo >b/another_file
ln -s file b/a_symlink
mkdir b/a_directory
mkdir b/another_directory
echo hello >b/another_directory/a_file

cp -r b c
echo new_text >c/file

# d is b with the file types all messed up:
mkdir d
mkdir d/file
echo basic >d/file/not_really
ln -s file d/another_file
echo hah >d/a_directory
mkdir -p d/another_directory/a_file
echo "something else" >d/another_directory/a_file/contents

cd ..

$OSTREE commit -b pristine/a --tree=dir=pristine/a
$OSTREE commit -b pristine/b --tree=dir=pristine/b
$OSTREE commit -b pristine/c --tree=dir=pristine/c
$OSTREE commit -b pristine/d --tree=dir=pristine/d

rm -rf pristine

mkdir realistic
cd realistic
mkdir -p etc var usr/lib usr/bin usr/share/doc/ostree
echo "ostree v1" >usr/bin/ostree
echo exe2 >usr/bin/exe2
echo exe3 >usr/bin/exe3

echo lib1 >usr/lib/libostree.so.0.1
ln -s libostree.so.0.1 usr/lib/libostree.so
echo lib2 >usr/lib/lib2.so.1.1
echo lib3 >usr/lib/lib3.so.1.3

echo "ostree is great" >usr/share/doc/ostree/README

$OSTREE commit -b realistic/a --tree=dir=.

# Install git
mkdir usr/share/doc/git
echo "git docs" >usr/share/doc/git/README
echo "git v1" >usr/bin/git

$OSTREE commit -b realistic/b --tree=dir=.

# Upgrade Ostree

echo "ostree v2" >usr/bin/ostree
rm usr/lib/libostree.so.0.1

echo "lib v2" >usr/lib/libostree.so.0.3
ln -sf libostree.so.0.3 usr/lib/libostree.so
echo "ostree is even better" >usr/share/doc/ostree/README
echo "<p>With more docs</p>" >usr/share/doc/ostree/README.html

$OSTREE commit -b realistic/c --tree=dir=.

cd ..

rm -rf realistic

##############

check_update_identical() {
    commit_a=$1
    commit_b=$2

    printf "\n\n### Test transitioning from %s to %s\n\n" "$commit_a" "$commit_b" >&2
    d=$(mktemp -d -p .)
    $OSTREE checkout $commit_a $d/a >&2
    $OSTREE checkout $commit_b $d/b >&2
    echo Expecting $commit_a to become $commit_b: >&2
    $OSTREE diff $commit_a $commit_b >&2
    $OSTREE checkout --update=$commit_a $commit_b $d/a >&2
    diff -r $d/a $d/b || fail >&2
}

check_update_identical pristine/a pristine/a
check_update_identical pristine/b pristine/b
check_update_identical pristine/c pristine/c
check_update_identical pristine/d pristine/d

check_update_identical realistic/a realistic/a
check_update_identical realistic/b realistic/b
check_update_identical realistic/c realistic/c


echo "ok Check overwriting checkout with no changes"

check_update_identical pristine/a pristine/b
check_update_identical pristine/b pristine/a
check_update_identical pristine/b pristine/c
check_update_identical pristine/c pristine/b
check_update_identical pristine/c pristine/d
check_update_identical pristine/d pristine/c

check_update_identical realistic/a realistic/b
check_update_identical realistic/b realistic/c
check_update_identical realistic/a realistic/c

echo "ok Check overwriting checkout with changes"
