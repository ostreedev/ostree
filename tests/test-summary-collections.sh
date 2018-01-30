#!/bin/bash
#
# Copyright © 2016 Red Hat, Inc.
# Copyright © 2017 Endless Mobile, Inc.
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

. $(dirname $0)/libtest.sh

echo '1..1'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo --collection-id org.example.Collection

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read i; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=repo commit --branch=test-$i -m test -s test tree
done

# Check that they are all listed, with the repository’s collection ID, in the summary.
${CMD_PREFIX} ostree --repo=repo summary --update

${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_file_has_content summary "(org.example.Collection, test-1)$"
assert_file_has_content summary "(org.example.Collection, test-2)$"
assert_file_has_content summary "(org.example.Collection, test-3)$"
assert_file_has_content summary "(org.example.Collection, test-4)$"
assert_file_has_content summary "(org.example.Collection, test-5)$"
assert_file_has_content summary "^Collection ID (ostree\.summary\.collection-id): org.example.Collection$"

# Test that mirrored branches are listed too.
${CMD_PREFIX} ostree --repo=repo refs --collections --create=org.example.OtherCollection:test-1-mirror test-1
${CMD_PREFIX} ostree --repo=repo summary --update

${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_file_has_content summary "(org.example.OtherCollection, test-1-mirror)$"

# Test that remote refs are listed, but only if they have collection IDs
cd ${test_tmpdir}
mkdir collection-repo
ostree_repo_init collection-repo --collection-id org.example.RemoteCollection
mkdir -p adir
${CMD_PREFIX} ostree --repo=collection-repo commit --branch=rcommit -m rcommit -s rcommit adir
${CMD_PREFIX} ostree --repo=repo remote add --no-gpg-verify --collection-id org.example.RemoteCollection collection-repo-remote "file://${test_tmpdir}/collection-repo"
${CMD_PREFIX} ostree --repo=repo pull collection-repo-remote rcommit
${CMD_PREFIX} ostree --repo=repo summary --update

${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_file_has_content summary "(org.example.RemoteCollection, rcommit)$"

cd ${test_tmpdir}
mkdir no-collection-repo
ostree_repo_init no-collection-repo
mkdir -p adir2
${CMD_PREFIX} ostree --repo=no-collection-repo commit --branch=rcommit2 -m rcommit2 -s rcommit2 adir2
${CMD_PREFIX} ostree --repo=repo remote add --no-gpg-verify no-collection-repo-remote "file://${test_tmpdir}/no-collection-repo"
${CMD_PREFIX} ostree --repo=repo pull no-collection-repo-remote rcommit2
${CMD_PREFIX} ostree --repo=repo summary --update

${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_not_file_has_content summary "rcommit2"

echo "ok summary collections"
