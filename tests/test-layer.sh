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

if ! ostree --version | grep -q -e '\+libarchive'; then
    echo "1..0 #SKIP no libarchive support compiled in"
    exit 0
fi

. $(dirname $0)/libtest.sh

echo "1..5"

setup_test_repository "archive-z2"

assert_valid_base_checkout () {
    cd $1
    assert_file_has_content test/change_me 1
    assert_file_has_content test/remove_me 2
    assert_file_has_content test/replace_me 3
    assert_file_has_content test/leave_me foo
    assert_not_has_file test/new

    assert_file_has_content test/dir_edit_me/change_me 1
    assert_file_has_content test/dir_edit_me/remove_me 2
    assert_file_has_content test/dir_edit_me/leave_me foo
    assert_not_has_file test/dir_edit_me/new
    assert_file_has_mode test/dir_edit_me 755

    assert_has_dir test/dir_remove_me
    assert_has_file test/dir_remove_me/a_file
    assert_has_dir test/dir_remove_me/subdir

    assert_has_dir test/dir_replace_me
    assert_has_file test/dir_replace_me/a_file
}  

assert_valid_layer_checkout () {
    cd $1
    
    assert_file_has_content test/change_me new
    assert_not_has_file test/remove_me 2
    assert_has_dir test/replace_me
    assert_has_file test/replace_me/new_file
    assert_file_has_content test/leave_me foo
    assert_has_file test/new

    assert_file_has_content test/dir_edit_me/change_me new
    assert_not_has_file test/dir_edit_me/remove_me
    assert_file_has_content test/dir_edit_me/leave_me foo
    assert_has_file test/dir_edit_me/new
    assert_file_has_mode test/dir_edit_me 775

    assert_not_has_dir test/dir_remove_me

    assert_has_file test/dir_replace_me
}  


$OSTREE commit -s "base" -b base  --owner-uid=`id -u` --owner-gid=`id -g` --tar-autocreate-parents \
  --tree=tar=${test_srcdir}/base.tar.gz

$OSTREE commit -s "layer" -b layer  --owner-uid=`id -u` --owner-gid=`id -g` --tar-autocreate-parents \
  --tree=tar=${test_srcdir}/layer.tar.gz

$OSTREE checkout -U base base-checkout
assert_valid_base_checkout base-checkout

echo "ok import base"

$OSTREE commit -s "layer1" -b layer1  --owner-uid=`id -u` --owner-gid=`id -g` --tar-autocreate-parents \
  --tree=tar=${test_srcdir}/base.tar.gz --tree=layer-tar=${test_srcdir}/layer.tar.gz

$OSTREE checkout -U layer1 layer1-checkout
assert_valid_layer_checkout layer1-checkout

echo "ok base-tar + layer-tar"

$OSTREE commit -s "layer2" -b layer2  --owner-uid=`id -u` --owner-gid=`id -g` --tar-autocreate-parents \
        --tree=ref=base --tree=layer-tar=${test_srcdir}/layer.tar.gz

$OSTREE checkout -U layer2 layer2-checkout
assert_valid_layer_checkout layer2-checkout

echo "ok base-ref + layer-tar"

$OSTREE commit -s "layer3" -b layer3  --owner-uid=`id -u` --owner-gid=`id -g` --tar-autocreate-parents \
  --tree=tar=${test_srcdir}/base.tar.gz --tree=layer-ref=layer

$OSTREE checkout -U layer3 layer3-checkout
assert_valid_layer_checkout layer3-checkout

echo "ok base-tar + layer-ref"

$OSTREE commit -s "layer4" -b layer4 \
  --tree=ref=base --tree=layer-ref=layer

$OSTREE checkout -U layer4 layer4-checkout
assert_valid_layer_checkout layer4-checkout

echo "ok base-ref + layer-ref"
