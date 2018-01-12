#!/bin/bash
#
# Copyright (C) 2014 Alexander Larsson <alexl@redhat.com>
# Copyright (C) 2018 Red Hat, Inc.
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

echo '1..4'

setup_test_repository "bare"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2 --mode="bare"

${CMD_PREFIX} ostree --repo=repo2 --untrusted pull-local repo

find repo2 -type f -links +1 | while read line; do
    assert_not_reached "pull-local created hardlinks"
done
echo "ok pull-local --untrusted didn't hardlink"

# Corrupt repo
for i in ${test_tmpdir}/repo/objects/*/*.file; do

    # make sure it's not a symlink
    if [ -L $i ]; then
        continue
    fi

    echo "corrupting $i"
    echo "broke" >> $i
    break;
done

rm -rf repo2
mkdir repo2
ostree_repo_init repo2 --mode="bare"
if ${CMD_PREFIX} ostree --repo=repo2 pull-local repo; then
    echo "ok trusted pull with corruption succeeded"
else
    assert_not_reached "corrupted trusted pull unexpectedly succeeded!"
fi

rm -rf repo2
ostree_repo_init repo2 --mode="bare"
if ${CMD_PREFIX} ostree --repo=repo2 pull-local --untrusted repo; then
    assert_not_reached "corrupted untrusted pull unexpectedly failed!"
else
    echo "ok untrusted pull with corruption failed"
fi


cd ${test_tmpdir}
tar xf ${test_srcdir}/ostree-path-traverse.tar.gz
rm -rf repo2
ostree_repo_init repo2 --mode=archive
if ${CMD_PREFIX} ostree --repo=repo2 pull-local --untrusted ostree-path-traverse/repo pathtraverse-test 2>err.txt; then
    fatal "pull-local unexpectedly succeeded"
fi
assert_file_has_content_literal err.txt 'Invalid / in filename ../afile'
echo "ok untrusted pull-local path traversal"
