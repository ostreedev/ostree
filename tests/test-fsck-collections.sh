#!/bin/bash
#
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

echo '1..11'

cd ${test_tmpdir}

# Create a new repository with one ref with the repository’s collection ID, and
# one ref with a different collection ID (which should be in refs/mirrors).
set_up_repo_with_collection_id() {
  rm -rf repo files

  mkdir repo
  ostree_repo_init repo --collection-id org.example.Collection

  mkdir files
  pushd files
  ${CMD_PREFIX} ostree --repo=../repo commit -s "Commit 1" -b ref1 > ../ref1-checksum
  ${CMD_PREFIX} ostree --repo=../repo commit -s "Commit 2" --orphan --bind-ref ref2 --add-metadata-string=ostree.collection-binding=org.example.Collection2 > ../ref2-checksum
  ${CMD_PREFIX} ostree --repo=../repo refs --collections --create=org.example.Collection2:ref2 $(cat ../ref2-checksum)
  popd
}

# Create a new repository with one ref and no collection IDs.
set_up_repo_without_collection_id() {
  rm -rf repo files

  mkdir repo
  ostree_repo_init repo

  mkdir files
  pushd files
  ${CMD_PREFIX} ostree --repo=../repo commit -s "Commit 3" -b ref3 --bind-ref ref4 > ../ref3-checksum
  ${CMD_PREFIX} ostree --repo=../repo refs --create=ref4 $(cat ../ref3-checksum)
  popd
}

set_up_repo_with_collection_id

# fsck at this point should succeed
${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs in collections...$"

# Drop the commit the ref points to, and drop the original ref so that fsck doesn’t prematurely fail on that.
find repo/objects -name '*.commit' -delete -print | wc -l > commitcount
assert_file_has_content commitcount "^2$"

rm repo/refs/heads/ref1

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo > fsck; then
    assert_not_reached "fsck unexpectedly succeeded after deleting commit!"
fi
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 1 fsck-collections"

# Try fsck in an old repository where refs/mirrors doesn’t exist to begin with.
# It should succeed.
set_up_repo_with_collection_id
rm -rf repo/refs/mirrors

${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 2 fsck-collections in old repository"

# Test that fsck detects commits which are pointed to by refs, but which don’t
# list those refs in their ref-bindings.
set_up_repo_with_collection_id
${CMD_PREFIX} ostree --repo=repo refs --create=new-ref $(cat ref1-checksum)

# For compatibility we don't check for this by default
${CMD_PREFIX} ostree fsck --repo=repo
# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Commit has no requested ref ‘new-ref’ in ref binding metadata (‘ref1’)"
assert_file_has_content fsck "^Validating refs...$"

echo "ok 3 fsck detects missing ref bindings"

# And the same where the ref is a collection–ref.
set_up_repo_with_collection_id
${CMD_PREFIX} ostree --repo=repo refs --collections --create=org.example.Collection2:new-ref $(cat ref1-checksum)

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Commit has no requested ref ‘new-ref’ in ref binding metadata (‘ref1’)"
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 4 fsck detects missing collection–ref bindings"

# Check that a ref with a different collection ID but the same ref name is caught.
set_up_repo_with_collection_id
${CMD_PREFIX} ostree --repo=repo refs --collections --create=org.example.Collection2:ref1 $(cat ref1-checksum)

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Commit has collection ID ‘org.example.Collection’ in collection binding metadata, while the remote it came from has collection ID ‘org.example.Collection2’"
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 5 fsck detects missing collection–ref bindings"

# Check that a commit with ref bindings which aren’t pointed to by refs is OK.
set_up_repo_with_collection_id
${CMD_PREFIX} ostree --repo=repo refs --delete ref1

# fsck at this point should succeed
${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 6 fsck ignores unreferenced ref bindings"

# …but it’s not OK if we pass --verify-back-refs to fsck.
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-back-refs > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Collection–ref (org.example.Collection, ref1) in bindings for commit .* does not exist"
assert_file_has_content fsck "^Validating refs...$"
assert_file_has_content fsck "^Validating refs in collections...$"

echo "ok 7 fsck ignores unreferenced ref bindings"

#
# Now repeat most of the above tests with a repository without collection IDs.
#

set_up_repo_without_collection_id

# fsck at this point should succeed
${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck
assert_file_has_content fsck "^Validating refs in collections...$"

# Drop the commit the ref points to, and drop the original ref so that fsck doesn’t prematurely fail on that.
find repo/objects -name '*.commit' -delete -print | wc -l > commitcount
assert_file_has_content commitcount "^1$"

rm repo/refs/heads/ref3

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck; then
    assert_not_reached "fsck unexpectedly succeeded after deleting commit!"
fi
assert_file_has_content fsck "^Validating refs...$"

echo "ok 8 fsck-collections"

# Test that fsck detects commits which are pointed to by refs, but which don’t
# list those refs in their ref-bindings.
set_up_repo_without_collection_id
${CMD_PREFIX} ostree --repo=repo refs --create=new-ref $(cat ref3-checksum)

# fsck should now fail
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-bindings > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Commit has no requested ref ‘new-ref’ in ref binding metadata (‘ref3’, ‘ref4’)"
assert_file_has_content fsck "^Validating refs...$"

echo "ok 9 fsck detects missing ref bindings"

# Check that a commit with ref bindings which aren’t pointed to by refs is OK.
set_up_repo_without_collection_id
${CMD_PREFIX} ostree --repo=repo refs --delete ref3

# fsck at this point should succeed
${CMD_PREFIX} ostree fsck --repo=repo > fsck
assert_file_has_content fsck "^Validating refs...$"

echo "ok 10 fsck ignores unreferenced ref bindings"

# …but it’s not OK if we pass --verify-back-refs to fsck.
if ${CMD_PREFIX} ostree fsck --repo=repo --verify-back-refs > fsck 2> fsck-error; then
    assert_not_reached "fsck unexpectedly succeeded after adding unbound ref!"
fi
assert_file_has_content fsck-error "Ref ‘ref3’ in bindings for commit .* does not exist"
assert_file_has_content fsck "^Validating refs...$"

echo "ok 11 fsck ignores unreferenced ref bindings"
