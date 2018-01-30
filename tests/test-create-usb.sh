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
#
# Authors:
#  - Philip Withnall <withnall@endlessm.com>

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..5"

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo --collection-id org.example.Collection1

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read i; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=repo commit --branch=test-$i -m test -s test  --gpg-homedir="${TEST_GPG_KEYHOME}" --gpg-sign="${TEST_GPG_KEYID_1}" tree
done

${CMD_PREFIX} ostree --repo=repo summary --update --gpg-homedir="${TEST_GPG_KEYHOME}" --gpg-sign="${TEST_GPG_KEYID_1}"

# Pull into a ‘local’ repository, to more accurately represent the situation of
# creating a USB stick from your local machine.
mkdir local-repo
${CMD_PREFIX} ostree --repo=local-repo init
${CMD_PREFIX} ostree --repo=local-repo remote add remote1 file://$(pwd)/repo --collection-id org.example.Collection1 --gpg-import="${test_tmpdir}/gpghome/key1.asc"
${CMD_PREFIX} ostree --repo=local-repo pull remote1 test-1 test-2 test-3 test-4 test-5

# Simple test to put two refs onto a USB stick.
mkdir dest-mount1
${CMD_PREFIX} ostree --repo=local-repo create-usb dest-mount1 org.example.Collection1 test-1 org.example.Collection1 test-2

assert_has_dir dest-mount1/.ostree/repo
${CMD_PREFIX} ostree --repo=dest-mount1/.ostree/repo refs --collections > dest-refs
assert_file_has_content dest-refs "^(org.example.Collection1, test-1)$"
assert_file_has_content dest-refs "^(org.example.Collection1, test-2)$"
assert_not_file_has_content dest-refs "^(org.example.Collection1, test-3)$"
assert_has_file dest-mount1/.ostree/repo/summary

echo "ok 1 simple usb"

# Test that the repository can be placed in another standard location on the USB stick.
for dest in ostree/repo .ostree/repo; do
    rm -rf dest-mount2
    mkdir dest-mount2
    ${CMD_PREFIX} ostree --repo=local-repo create-usb --destination-repo "$dest" dest-mount2 org.example.Collection1 test-1
    assert_has_dir "dest-mount2/$dest"
    if [ -d dest-mount2/.ostree/repos.d ]; then
        ls dest-mount2/.ostree/repos.d | wc -l > repo-links
        assert_file_has_content repo-links "^0$"
    fi
done

echo "ok 2 usb in standard location"

# Test that the repository can be placed in a non-standard location and gets a symlink to it.
mkdir dest-mount3
${CMD_PREFIX} ostree --repo=local-repo create-usb --destination-repo some-dest dest-mount3 org.example.Collection1 test-1
assert_has_dir "dest-mount3/some-dest"
assert_symlink_has_content "dest-mount3/.ostree/repos.d/00-generated" "/some-dest$"
${CMD_PREFIX} ostree --repo=dest-mount3/.ostree/repos.d/00-generated refs --collections > dest-refs
assert_file_has_content dest-refs "^(org.example.Collection1, test-1)$"
assert_has_file dest-mount3/.ostree/repos.d/00-generated/summary

echo "ok 3 usb in non-standard location"

# Test that adding an additional ref to an existing USB repository works.
${CMD_PREFIX} ostree --repo=local-repo create-usb --destination-repo some-dest dest-mount3 org.example.Collection1 test-2 org.example.Collection1 test-3
assert_has_dir "dest-mount3/some-dest"
assert_symlink_has_content "dest-mount3/.ostree/repos.d/00-generated" "/some-dest$"
${CMD_PREFIX} ostree --repo=dest-mount3/.ostree/repos.d/00-generated refs --collections > dest-refs
assert_file_has_content dest-refs "^(org.example.Collection1, test-1)$"
assert_file_has_content dest-refs "^(org.example.Collection1, test-1)$"
assert_file_has_content dest-refs "^(org.example.Collection1, test-3)$"
assert_has_file dest-mount3/.ostree/repos.d/00-generated/summary

echo "ok 4 adding ref to an existing usb"

# Check that #OstreeRepoFinderMount works from a volume initialised uing create-usb.
mkdir finder-repo
ostree_repo_init finder-repo
${CMD_PREFIX} ostree --repo=finder-repo remote add remote1 file://$(pwd)/just-needed-for-the-keyring --collection-id org.example.Collection1 --gpg-import="${test_tmpdir}/gpghome/key1.asc"

${test_builddir}/repo-finder-mount finder-repo dest-mount1 org.example.Collection1 test-1 org.example.Collection1 test-2 &> out
assert_file_has_content out "^0 .*_2Fdest-mount1_2F.ostree_2Frepo_remote1.trustedkeys.gpg org.example.Collection1 test-1 $(ostree --repo=repo show test-1)$"
assert_file_has_content out "^0 .*_2Fdest-mount1_2F.ostree_2Frepo_remote1.trustedkeys.gpg org.example.Collection1 test-2 $(ostree --repo=repo show test-2)$"

echo "ok 5 find from usb repo"
