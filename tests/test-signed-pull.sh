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

echo "1..4"

setup_fake_remote_repo1 "archive"

repo_mode="archive"

function repo_init() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ostree_repo_init repo --mode=${repo_mode}
    ${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo "$@"
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
test_signed_pull "dummy"


# Test ostree sign with 'ed25519' module
# Generate private key in PEM format
PEMFILE="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.pem)"
openssl genpkey -algorithm ed25519 -outform PEM -out "${PEMFILE}"

# Based on: http://openssl.6102.n7.nabble.com/ed25519-key-generation-td73907.html
# Extract the private and public parts from generated key.
PUBLIC="$(openssl pkey -outform DER -pubout -in ${PEMFILE} | tail -c 32 | base64)"
SEED="$(openssl pkey -outform DER -in ${PEMFILE} | tail -c 32 | base64)"
# Secret key is concantination of SEED and PUBLIC
SECRET="$(echo ${SEED}${PUBLIC} | base64 -d | base64 -w 0)"

echo "SEED = $SEED"
echo "PUBLIC = $PUBLIC"

COMMIT_ARGS="--sign=${SECRET} --sign-type=ed25519"

repo_init --set=sign-verify=true
${CMD_PREFIX} ostree --repo=repo config set 'remote "origin"'.verification-key "${PUBLIC}"
test_signed_pull "ed25519"

