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

echo "1..8"

mkdir ${test_tmpdir}/repo
ostree_repo_init repo --mode="archive"

echo "Unsigned commit" > file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Test `ostree sign` with dummy module first
DUMMYSIGN="dummysign"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy ${COMMIT} ${DUMMYSIGN}

# Ensure that detached metadata really contain expected string
EXPECTEDSIGN="$(echo $DUMMYSIGN | hexdump -n 9 -e '8/1 "0x%.2x, " 1/1 " 0x%.2x"')"
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.dummy | grep -q -e "${EXPECTEDSIGN}"
echo "ok Detached dummy signature added"

# Verify vith sign mechanism
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN}
echo "ok dummy signature verified"

echo "Signed commit with dummy key: ${DUMMYSIGN}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Signed with dummy module' --sign=${DUMMYSIGN} --sign-type=dummy 
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN}
echo "ok commit with dummy signing"

# tests below require libsodium support
if ! has_libsodium; then
    echo "ok Detached ed25519 signature # SKIP due libsodium unavailability"
    echo "ok ed25519 signature verified # SKIP due libsodium unavailability"
    echo "ok multiple signing # SKIP due libsodium unavailability"
    echo "ok verify ed25519 keys file # SKIP due libsodium unavailability"
    echo "ok sign with ed25519 keys file # SKIP due libsodium unavailability"
    exit 0
fi

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

echo "Signed commit with ed25519: ${SECRET}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s "Signed with ed25519 module" --sign="${SECRET}" --sign-type=ed25519
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Ensure that detached metadata contain signature
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.ed25519 &>/dev/null
echo "ok Detached ed25519 signature added"

# Verify vith sign mechanism
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC}
echo "ok ed25519 signature verified"

# Check if we able to use all available modules to sign the same commit
echo "Unsigned commit for multi-sign" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"
# Check if we have no signatures
for mod in "dummy" "ed25519"; do
    if ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.${mod}; then
        echo "Unexpected signature for ${mod} found"
        exit 1
    fi
done

# Sign with all available modules
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy ${COMMIT} ${DUMMYSIGN}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=ed25519 ${COMMIT} ${SECRET}
# and verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN}
echo "ok multiple signing "

# Prepare files with public ed25519 signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.ed25519)"

# Test if file contain no keys
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT}; then
    exit 1
fi
# Test if have a problem with file object
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
# Test with single key in list
echo ${PUBLIC} > ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT}

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    openssl genpkey -algorithm ED25519 | openssl pkey -outform DER | tail -c 32 | base64
done > ${PUBKEYS}
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT}; then
    exit 1
fi

# Add correct key into the list
echo ${PUBLIC} >> ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT}

echo "ok verify ed25519 keys file"

# Check ed25519 signing with secret file
echo "Unsigned commit for secret file usage" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

KEYFILE="$(mktemp -p ${test_tmpdir} secret_XXXXXX.ed25519)"
echo "${SECRET}" > ${KEYFILE}
# Sign
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=ed25519 --keys-file=${KEYFILE} ${COMMIT}
# Verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT}
echo "ok sign with ed25519 keys file"

