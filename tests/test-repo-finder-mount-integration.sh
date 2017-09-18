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
#
# Authors:
#  - Philip Withnall <withnall@endlessm.com>

set -euo pipefail

. $(dirname $0)/libtest.sh

SUDO="sudo --non-interactive"

# Skip the test if a well-known USB stick is not available.
# To run the test, you must have a throwaway partition on a USB stick (doesn’t
# matter how it’s formatted, as the test will reformat it), and must set
# MOUNT_INTEGRATION_DEV to that partition. For example,
# MOUNT_INTEGRATION_DEV=/dev/sdb1. For safety, the test will be skipped if the
# partition is already mounted.
#
# FIXME: We could potentially automate this in future if there’s a way to trick
# GIO into treating an arbitrary partition (such as a loopback device of our
# creation) as removable.

if ! [ -b "${MOUNT_INTEGRATION_DEV:-}" ]; then
    skip "Test needs a disposable USB stick passed in as MOUNT_INTEGRATION_DEV"
fi

# Sanity check that the given device is not already mounted, to try and avoid
# hosing the system.
if mount | grep -q "${MOUNT_INTEGRATION_DEV}"; then
    skip "${MOUNT_INTEGRATION_DEV} must not be mounted already"
fi

_mount_cleanup () {
    ${SUDO} umount "${MOUNT_INTEGRATION_DEV}" || true
}

case "${TEST_SKIP_CLEANUP:-}" in
    no|"")
        trap _mount_cleanup EXIT
        ;;
    err)
        trap _mount_cleanup ERR
        ;;
esac

echo "1..3"

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
${CMD_PREFIX} ostree --repo=repo rev-parse test-1 > ref1-checksum

# Pull into a ‘local’ repository, to more accurately represent the situation of
# creating a USB stick from your local machine.
mkdir local-repo
ostree_repo_init local-repo
${CMD_PREFIX} ostree --repo=local-repo remote add remote1 file://$(pwd)/repo --collection-id org.example.Collection1 --gpg-import="${test_tmpdir}/gpghome/key1.asc"
${CMD_PREFIX} ostree --repo=local-repo pull remote1 test-1 test-2 test-3 test-4 test-5

for fs_type in ext4 vfat; do
    # Prepare a USB stick containing some of the refs on the given file system.
    if [ "$fs_type" = "ext4" ]; then
        fs_options="-F -E root_owner=$(id -u):$(id -g)"
    else
        fs_options=
    fi
    ${SUDO} mkfs.$fs_type $fs_options "${MOUNT_INTEGRATION_DEV}" > /dev/null
    usb_mount=$(udisksctl mount --block-device "${MOUNT_INTEGRATION_DEV}" --filesystem-type $fs_type | sed -n "s/^Mounted .* at \(.*\)\.$/\1/p")

    ${CMD_PREFIX} ostree --repo=local-repo create-usb "${usb_mount}" org.example.Collection1 test-1 org.example.Collection1 test-2

    assert_has_dir "${usb_mount}"/.ostree/repo
    ${CMD_PREFIX} ostree --repo="${usb_mount}"/.ostree/repo refs --collections > dest-refs
    assert_file_has_content dest-refs "^(org.example.Collection1, test-1)$"
    assert_file_has_content dest-refs "^(org.example.Collection1, test-2)$"
    assert_not_file_has_content dest-refs "^(org.example.Collection1, test-3)$"
    assert_has_file "${usb_mount}"/.ostree/repo/summary

    # Pull into a second local repository (theoretically, a separate computer).
    mkdir peer-repo_$fs_type
    ostree_repo_init peer-repo_$fs_type
    ${CMD_PREFIX} ostree --repo=peer-repo_$fs_type remote add remote1 file://just-here-for-the-keyring --collection-id org.example.Collection1 --gpg-import="${test_tmpdir}/gpghome/key1.asc"

    ${CMD_PREFIX} ostree --repo=peer-repo_$fs_type find-remotes org.example.Collection1 test-1 > find-results
    assert_not_file_has_content find-results "^No results.$"
    assert_file_has_content find-results "^Result 0: file://${usb_mount}"
    assert_file_has_content find-results "(org.example.Collection1, test-1) = $(cat ref1-checksum)$"

    ${CMD_PREFIX} ostree --repo=peer-repo_$fs_type find-remotes --pull org.example.Collection1 test-1 > pull-results
    assert_file_has_content pull-results "^Pulled 1/1 refs successfully.$"

    ${CMD_PREFIX} ostree --repo=peer-repo_$fs_type refs --collections > refs
    assert_file_has_content refs "^(org.example.Collection1, test-1)$"

    ${SUDO} umount "${MOUNT_INTEGRATION_DEV}"

    echo "ok end-to-end USB on ${fs_type}"
done

# Check the two repositories are identical.
diff -ur peer-repo_ext4 peer-repo_vfat

echo "ok end-to-end USB repositories are identical"
