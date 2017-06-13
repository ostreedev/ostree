#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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

echo '1..2'

cd ${test_tmpdir}

# Check that refs with collection IDs (i.e. refs in refs/mirrors) are taken into account
# when computing reachability of objects while pruning.
set_up_repo() {
  rm -rf repo files

  mkdir repo
  ostree_repo_init repo

  mkdir files
  pushd files
  ${CMD_PREFIX} ostree --repo=../repo commit -s "Commit 1" -b original-ref > ../original-ref-checksum
  popd
  ${CMD_PREFIX} ostree --repo=repo refs --collections --create=org.example.Collection:some-ref $(cat original-ref-checksum)
}

set_up_repo

# Try deleting a specific commit which is still pointed to by both refs.
if ${CMD_PREFIX} ostree --repo=repo prune --delete-commit=$(cat original-ref-checksum) 2>/dev/null; then
    assert_not_reached "prune unexpectedly succeeded in deleting a referenced commit!"
fi

# Pruning normally should do nothing.
${CMD_PREFIX} ostree --repo=repo prune --refs-only > prune
assert_file_has_content prune "^Total objects: 3$"
assert_file_has_content prune "^No unreachable objects$"

# Remove the original-ref so that only the some-ref with a collection ID points to the commit.
${CMD_PREFIX} ostree --repo=repo refs --delete original-ref

${CMD_PREFIX} ostree --repo=repo prune --refs-only > prune
assert_file_has_content prune "^Total objects: 3$"
assert_file_has_content prune "^No unreachable objects$"

# Remove the second ref so that the commit is now orphaned.
${CMD_PREFIX} ostree --repo=repo refs --collections --delete org.example.Collection

${CMD_PREFIX} ostree --repo=repo prune --refs-only > prune
assert_file_has_content prune "^Total objects: 3$"
assert_file_has_content prune "^Deleted 3 objects, [0-9]\+ bytes freed$"

echo "ok 1 prune-collections"

# Try again, but in an old repository where refs/mirrors doesn’t exist to begin with.
set_up_repo
rm -rf repo/refs/mirrors

${CMD_PREFIX} ostree --repo=repo prune --refs-only > prune
assert_file_has_content prune "^Total objects: 3$"
assert_file_has_content prune "^No unreachable objects$"

echo "ok 2 prune-collections in old repository"
