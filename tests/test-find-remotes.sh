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

echo '1..1'

# Create two upstream collection repositories with some example commits
cd ${test_tmpdir}

mkdir apps-collection
ostree_repo_init apps-collection --collection-id org.example.AppsCollection
mkdir -p files
pushd files
${CMD_PREFIX} ostree --repo=../apps-collection commit -s "Test apps-collection commit 1" -b app1 --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1} > ../app1-checksum
${CMD_PREFIX} ostree --repo=../apps-collection commit -s "Test apps-collection commit 2" -b app2 --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1} > ../app2-checksum
popd
${CMD_PREFIX} ostree --repo=apps-collection summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}

mkdir os-collection
ostree_repo_init os-collection --collection-id org.example.OsCollection
mkdir -p files
pushd files
${CMD_PREFIX} ostree --repo=../os-collection commit -s "Test os-collection commit 1" -b os/amd64/master --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2} > ../os-checksum
popd
${CMD_PREFIX} ostree --repo=os-collection summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2}

# Create a local repository where we pull the branches from the two remotes as normal, using GPG.
mkdir local
ostree_repo_init local
${CMD_PREFIX} ostree --repo=local remote add apps-remote file://$(pwd)/apps-collection --collection-id org.example.AppsCollection --gpg-import=${test_tmpdir}/gpghome/key1.asc
${CMD_PREFIX} ostree --repo=local remote add os-remote file://$(pwd)/os-collection --collection-id org.example.OsCollection --gpg-import=${test_tmpdir}/gpghome/key2.asc

${CMD_PREFIX} ostree --repo=local pull apps-remote app1
${CMD_PREFIX} ostree --repo=local pull os-remote os/amd64/master

${CMD_PREFIX} ostree --repo=local refs > refs
assert_file_has_content refs "^apps-remote:app1$"
assert_file_has_content refs "^os-remote:os/amd64/master$"

${CMD_PREFIX} ostree --repo=local refs --collections > refs
cat refs | wc -l > refscount
assert_file_has_content refs "^(org.example.AppsCollection, app1)$"
assert_file_has_content refs "^(org.example.OsCollection, os/amd64/master)$"
assert_file_has_content refscount "^2$"

# Create a local mirror repository where we pull the branches *in mirror mode* from the two remotes.
# This should pull them into refs/mirrors, since the remotes advertise a collection ID.
mkdir local-mirror
ostree_repo_init local-mirror
${CMD_PREFIX} ostree --repo=local-mirror remote add apps-remote file://$(pwd)/apps-collection --collection-id org.example.AppsCollection --gpg-import=${test_tmpdir}/gpghome/key1.asc
${CMD_PREFIX} ostree --repo=local-mirror remote add os-remote file://$(pwd)/os-collection --collection-id org.example.OsCollection --gpg-import=${test_tmpdir}/gpghome/key2.asc

${CMD_PREFIX} ostree --repo=local-mirror pull --mirror apps-remote app1
${CMD_PREFIX} ostree --repo=local-mirror pull --mirror os-remote os/amd64/master

${CMD_PREFIX} ostree --repo=local-mirror refs | wc -l > refscount
assert_file_has_content refscount "^0$"
ls -1 local-mirror/refs/remotes | wc -l > remotescount
assert_file_has_content remotescount "^0$"

${CMD_PREFIX} ostree --repo=local-mirror refs --collections > refs
assert_file_has_content refs "^(org.example.AppsCollection, app1)$"
assert_file_has_content refs "^(org.example.OsCollection, os/amd64/master)$"

assert_file_has_content local-mirror/refs/mirrors/org.example.AppsCollection/app1 "^$(cat app1-checksum)$"
assert_file_has_content local-mirror/refs/mirrors/org.example.OsCollection/os/amd64/master "^$(cat os-checksum)$"

for repo in local local-mirror; do
    # Try finding an update for an existing branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.AppsCollection app1 > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/apps-collection$"
    assert_file_has_content find "^ - Keyring: apps-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.AppsCollection, app1) = $(cat app1-checksum)$"
    assert_file_has_content find "^1/1 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Find several updates for several existing branches.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.AppsCollection app1 org.example.OsCollection os/amd64/master > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/apps-collection$"
    assert_file_has_content find "^ - Keyring: apps-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.AppsCollection, app1) = $(cat app1-checksum)$"
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/os-collection$"
    assert_file_has_content find "^ - Keyring: os-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum)$"
    assert_file_has_content find "^2/2 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Find some updates and a new branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.AppsCollection app1 org.example.AppsCollection app2 org.example.OsCollection os/amd64/master > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/apps-collection$"
    assert_file_has_content find "^ - Keyring: apps-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.AppsCollection, app1) = $(cat app1-checksum)$"
    assert_file_has_content find "^    - (org.example.AppsCollection, app2) = $(cat app2-checksum)$"
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/os-collection$"
    assert_file_has_content find "^ - Keyring: os-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum)$"
    assert_file_has_content find "^3/3 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Find an update and a non-existent branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.AppsCollection app1 org.example.AppsCollection not-an-app > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/apps-collection$"
    assert_file_has_content find "^ - Keyring: apps-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.AppsCollection, not-an-app) = (not found)$"
    assert_file_has_content find "^    - (org.example.AppsCollection, app1) = $(cat app1-checksum)$"
    assert_file_has_content find "^Refs not found in any remote:$"
    assert_file_has_content find "^ - (org.example.AppsCollection, not-an-app)$"
    assert_file_has_content find "^1/2 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Do all the above, but pull this time.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.AppsCollection app1 > pull || true
    assert_file_has_content pull "^1/1 refs were found.$"
    assert_file_has_content pull "^Pulled 1/1 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo app1 $(cat app1-checksum)

    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.AppsCollection app1 org.example.OsCollection os/amd64/master > pull
    assert_file_has_content pull "^2/2 refs were found.$"
    assert_file_has_content pull "^Pulled 2/2 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo app1 $(cat app1-checksum)
    assert_ref $repo os/amd64/master $(cat os-checksum)

    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.AppsCollection app1 org.example.AppsCollection app2 org.example.OsCollection os/amd64/master > pull
    assert_file_has_content pull "^3/3 refs were found.$"
    assert_file_has_content pull "^Pulled 3/3 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo app1 $(cat app1-checksum)
    assert_ref $repo app2 $(cat app2-checksum)
    assert_ref $repo os/amd64/master $(cat os-checksum)

    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.AppsCollection app1 org.example.AppsCollection not-an-app > pull
    assert_file_has_content pull "^1/2 refs were found.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo app1 $(cat app1-checksum)
    assert_not_ref $repo not-an-app
done

# Test pulling a new commit into the local mirror from one of the repositories.
pushd files
${CMD_PREFIX} ostree --repo=../os-collection commit -s "Test os-collection commit 2" -b os/amd64/master --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2} > ../os-checksum-2
popd
${CMD_PREFIX} ostree --repo=os-collection summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2}

for repo in local-mirror; do
    # Try finding an update for that branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.OsCollection os/amd64/master > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/os-collection$"
    assert_file_has_content find "^ - Keyring: os-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum-2)$"
    assert_file_has_content find "^1/1 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Pull it.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.OsCollection os/amd64/master > pull || true
    assert_file_has_content pull "^1/1 refs were found.$"
    assert_file_has_content pull "^Pulled 1/1 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo os/amd64/master $(cat os-checksum-2)

    # We need to manually update the refs afterwards, since the original pull
    # into the local-mirror was a --mirror pull — so it wrote refs/mirrors/blah.
    # This pull was not, so it wrote refs/remotes/blah.
    ${CMD_PREFIX} ostree --repo=$repo refs --collections --create org.example.OsCollection:os/amd64/master os-remote:os/amd64/master
done

# Add the local mirror to the local repository as a remote, so that the local repo
# has two configured remotes for the os-collection. Ensure its summary is up to date first.
${CMD_PREFIX} ostree --repo=local-mirror summary --update
${CMD_PREFIX} ostree --repo=local remote add os-remote-local-mirror file://$(pwd)/local-mirror --collection-id org.example.OsCollection --gpg-import=${test_tmpdir}/gpghome/key2.asc

for repo in local; do
    # Try finding an update for that branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.OsCollection os/amd64/master > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/os-collection$"
    assert_file_has_content find "^ - Keyring: os-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum-2)$"
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/local-mirror$"
    assert_file_has_content find "^ - Keyring: os-remote-local-mirror.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum-2)$"
    assert_file_has_content find "^1/1 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Pull it.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.OsCollection os/amd64/master > pull || true
    assert_file_has_content pull "^1/1 refs were found.$"
    assert_file_has_content pull "^Pulled 1/1 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo os/amd64/master $(cat os-checksum-2)
done

# Add another commit to the OS collection, but don’t update the mirror. Then try pulling
# into the local repository again, and check that the outdated ref in the mirror is ignored.
pushd files
${CMD_PREFIX} ostree --repo=../os-collection commit -s "Test os-collection commit 3" -b os/amd64/master --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2} > ../os-checksum-3
popd
${CMD_PREFIX} ostree --repo=os-collection summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_2}

for repo in local; do
    # Try finding an update for that branch.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes org.example.OsCollection os/amd64/master > find
    assert_file_has_content find "^Result [0-9]\+: file://$(pwd)/os-collection$"
    assert_file_has_content find "^ - Keyring: os-remote.trustedkeys.gpg$"
    assert_file_has_content find "^    - (org.example.OsCollection, os/amd64/master) = $(cat os-checksum-3)$"
    assert_file_has_content find "^1/1 refs were found.$"
    assert_not_file_has_content find "^No results.$"

    # Pull it.
    ${CMD_PREFIX} ostree --repo=$repo find-remotes --pull org.example.OsCollection os/amd64/master > pull || true
    assert_file_has_content pull "^1/1 refs were found.$"
    assert_file_has_content pull "^Pulled 1/1 refs successfully.$"
    assert_not_file_has_content pull "Failed to pull some refs from the remotes"
    assert_ref $repo os/amd64/master $(cat os-checksum-3)
done

echo "ok find-remotes"
