#!/bin/bash
#
# Copyright (C) 2019 Collabora Ltd.
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

echo "1..20"

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1
setup_fake_remote_repo1 "archive"

repo_mode="archive"

function repo_init() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ostree_repo_init repo --mode=${repo_mode}
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false --set=sign-verify-summary=false origin $(cat httpd-address)/ostree/gnomerepo "$@"
}

function test_signed_pull() {
    local sign_type="$1"
    local comment="$2"
    cd ${test_tmpdir}
    ${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} \
        -b main -s "A signed commit" --tree=ref=main

    ${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
    # make sure gpg verification is correctly on
    csum=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo rev-parse main)
    objpath=objects/${csum::2}/${csum:2}.commitmeta
    remotesig=ostree-srv/gnomerepo/$objpath
    localsig=repo/$objpath
    mv $remotesig $remotesig.bak
    if ${CMD_PREFIX} ostree --repo=repo --depth=0 pull origin main; then
        assert_not_reached "pull with sign-verify and no commitmeta unexpectedly succeeded?"
    fi
    # ok now check that we can pull correctly
    mv $remotesig.bak $remotesig
    ${CMD_PREFIX} ostree --repo=repo pull origin main
    echo "ok ${sign_type}${comment} pull signed commit"
    rm $localsig
    ${CMD_PREFIX} ostree --repo=repo pull origin main
    test -f $localsig
    echo "ok ${sign_type}${comment} re-pull signature for stored commit"
}

DUMMYSIGN="dummysign"
COMMIT_ARGS="--sign=${DUMMYSIGN} --sign-type=dummy"
repo_init --set=sign-verify=true

# Check if verification-key and verification-file options throw error with wrong keys
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} \
    -b main -s "A signed commit" --tree=ref=main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
    assert_not_reached "pull without keys unexpectedly succeeded"
fi
echo "ok pull failure without keys preloaded"

${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-dummy-key "somewrongkey"
if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
    assert_not_reached "pull with unknown key unexpectedly succeeded"
fi
echo "ok pull failure with incorrect key option"

${CMD_PREFIX} ostree --repo=repo config unset 'remote "origin"'.verification-dummy-key
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-dummy-file "/non/existing/file"
if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
    assert_not_reached "pull with unknown keys file unexpectedly succeeded"
fi
echo "ok pull failure with incorrect keys file option"

# Test with correct dummy key
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-dummy-key "${DUMMYSIGN}"
${CMD_PREFIX} ostree --repo=repo config unset 'remote "origin"'.verification-dummy-file
test_signed_pull "dummy" ""

# Another test with the --verify option directly
repo_init --sign-verify=dummy=inline:${DUMMYSIGN}
test_signed_pull "dummy" "from remote opt"

# And now explicitly limit it to dummy
repo_init
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.sign-verify dummy
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-dummy-key "${DUMMYSIGN}"
test_signed_pull "dummy" "explicit value"

# dummy, but no key configured
repo_init
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.sign-verify dummy
if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>err.txt; then
    assert_not_reached "pull with nosuchsystem succeeded"
fi
assert_file_has_content err.txt 'No keys found for required signapi type dummy'
echo "ok explicit dummy but unconfigured"

# Set it to an unknown explicit value
repo_init
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.sign-verify nosuchsystem;
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-dummy-key "${DUMMYSIGN}"
if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>err.txt; then
    assert_not_reached "pull with nosuchsystem succeeded"
fi
assert_file_has_content err.txt 'Requested signature type is not implemented'
echo "ok pull failure for unknown system"

repo_init
if ${CMD_PREFIX} ostree --repo=repo remote add other --sign-verify=trustme=inline:ok http://localhost 2>err.txt; then
    assert_not_reached "remote add with invalid keytype succeeded"
fi
assert_file_has_content err.txt 'Requested signature type is not implemented'
if ${CMD_PREFIX} ostree --repo=repo remote add other --sign-verify=dummy http://localhost 2>err.txt; then
    assert_not_reached "remote add with invalid keytype succeeded"
fi
assert_file_has_content err.txt 'Failed to parse KEYTYPE'
if ${CMD_PREFIX} ostree --repo=repo remote add other --sign-verify=dummy=foo:bar http://localhost 2>err.txt; then
    assert_not_reached "remote add with invalid keytype succeeded"
fi
assert_file_has_content err.txt 'Invalid key reference'
echo "ok remote add errs"

if ! has_sign_ed25519; then
    echo "ok ed25519-key pull signed commit # SKIP due libsodium unavailability"
    echo "ok ed25519-key re-pull signature for stored commit # SKIP due libsodium unavailability"
    echo "ok ed25519-key+file pull signed commit # SKIP due libsodium unavailability"
    echo "ok ed25519-key+file re-pull signature for stored commit # SKIP due libsodium unavailability"
    echo "ok ed25519-file pull signed commit # SKIP due libsodium unavailability"
    echo "ok ed25519-file re-pull signature for stored commit # SKIP due libsodium unavailability"
    echo "ok ed25519-inline # SKIP due libsodium unavailability"
    echo "ok ed25519-inline # SKIP due libsodium unavailability"
    exit 0
fi

# Test ostree sign with 'ed25519' module
gen_ed25519_keys
PUBLIC=${ED25519PUBLIC}
SEED=${ED25519SEED}
SECRET=${ED25519SECRET}

COMMIT_ARGS="--sign=${SECRET} --sign-type=ed25519"

repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-ed25519-key "${PUBLIC}"
test_signed_pull "ed25519" "key"

# Prepare files with public ed25519 signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.ed25519)"

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_ed25519_random_public
done > ${PUBKEYS}

# Test case with the file containing incorrect signatures and with the correct key set
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-ed25519-file "${PUBKEYS}"
test_signed_pull "ed25519" "key+file"

# Add correct key into the list
echo ${PUBLIC} >> ${PUBKEYS}

repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-ed25519-file "${PUBKEYS}"
test_signed_pull "ed25519" "file"

repo_init --sign-verify=ed25519=inline:"${ED25519PUBLIC}"
test_signed_pull "ed25519" "--verify-ed25519"
