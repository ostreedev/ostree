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

echo "1..7"

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
        assert_not_reached "pull with sign-verify unexpectedly succeeded?"
    fi
    # ok now check that we can pull correctly
    mv $remotesig.bak $remotesig
    ${CMD_PREFIX} ostree --repo=repo pull origin main
    echo "ok pull ${sign_type} signed commit"
    rm $localsig
    ${CMD_PREFIX} ostree --repo=repo pull origin main
    test -f $localsig
    echo "ok re-pull ${sign_type} signature for stored commit"
}

DUMMYSIGN="dummysign"
COMMIT_ARGS="--sign=${DUMMYSIGN} --sign-type=dummy"
repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-key "${DUMMYSIGN}"
test_signed_pull "dummy"


# Test ostree sign with 'ed25519' module
gen_ed25519_keys
PUBLIC=${ED25519PUBLIC}
SEED=${ED25519SEED}
SECRET=${ED25519SECRET}

COMMIT_ARGS="--sign=${SECRET} --sign-type=ed25519"

repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-key "${PUBLIC}"
test_signed_pull "ed25519"

# Prepare files with public ed25519 signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.ed25519)"

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_ed25519_random_public
done > ${PUBKEYS}
# Add correct key into the list
echo ${PUBLIC} >> ${PUBKEYS}

repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-file "${PUBKEYS}"
test_signed_pull "ed25519"

echo "ok verify ed25519 keys file"
