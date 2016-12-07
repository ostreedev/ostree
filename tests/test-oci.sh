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

. $(dirname $0)/libtest.sh

setup_test_repository "bare"

echo "1..2"

$OSTREE export --oci --oci-tag=exported --output=oci-dir test2

assert_has_file oci-dir/oci-layout
assert_has_dir oci-dir/blobs/sha256
assert_has_dir oci-dir/refs
assert_file_has_content oci-dir/refs/exported "application/vnd.oci.image.manifest.v1+json"

for i in oci-dir/blobs/sha256/*; do
     echo $(basename $i) $i >> sums
done
sha256sum -c sums
    
echo "ok export oci"

$OSTREE --repo=repo2 init

$OSTREE commit --repo=repo2 --oci --oci-tag=exported --branch=imported oci-dir

$OSTREE checkout --repo=repo2 imported checked-out

assert_has_file checked-out/firstfile
assert_has_file checked-out/baz/cow
assert_file_has_content checked-out/baz/cow moo
assert_has_file checked-out/baz/deeper/ohyeah

echo "ok commit oci"



