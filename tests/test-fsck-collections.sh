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

# Check that fsck detects errors with refs which have collection IDs (i.e. refs in refs/mirrors).
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

# fsck at this point should succeed
${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs in collections...$"

# Drop the commit the ref points to, and drop the original ref so that fsck doesn’t prematurely fail on that.
find repo/objects -name '*.commit' -delete -print | wc -l > commitcount
assert_file_has_content commitcount "^1$"

rm repo/refs/heads/original-ref

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo > fsck; then
    assert_not_reached "fsck unexpectedly succeeded after deleting commit!"
fi
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 1 fsck-collections"

# Try fsck in an old repository where refs/mirrors doesn’t exist to begin with.
# It should succeed.
set_up_repo
rm -rf repo/refs/mirrors

${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 2 fsck-collections in old repository"
