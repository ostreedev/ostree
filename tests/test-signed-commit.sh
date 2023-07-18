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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..11"

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1

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

if ${CMD_PREFIX} env -u OSTREE_DUMMY_SIGN_ENABLED ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN} 2>err.txt; then
    fatal "verified dummy signature without env"
fi
# FIXME the error message here is broken
#assert_file_has_content_literal err.txt 'dummy signature type is only for ostree testing'
assert_file_has_content_literal err.txt ' No valid signatures found'
echo "ok dummy sig requires env"

# tests below require libsodium support
if ! has_sign_ed25519; then
    echo "ok Detached ed25519 signature # SKIP due libsodium unavailability"
    echo "ok ed25519 signature verified # SKIP due libsodium unavailability"
    echo "ok multiple signing # SKIP due libsodium unavailability"
    echo "ok verify ed25519 keys file # SKIP due libsodium unavailability"
    echo "ok sign with ed25519 keys file # SKIP due libsodium unavailability"
    echo "ok verify ed25519 system-wide configuration # SKIP due libsodium unavailability"
    echo "ok verify ed25519 revoking keys mechanism # SKIP due libsodium unavailability"
    exit 0
fi

# Test ostree sign with 'ed25519' module
gen_ed25519_keys
PUBLIC=${ED25519PUBLIC}
SECRET=${ED25519SECRET}

WRONG_PUBLIC="$(gen_ed25519_random_public)"

echo "PUBLIC = $PUBLIC"

echo "Signed commit with ed25519: ${SECRET}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s "Signed with ed25519 module" --sign="${SECRET}" --sign-type=ed25519
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Ensure that detached metadata contain signature
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.ed25519 &>/dev/null
echo "ok Detached ed25519 signature added"

# Verify vith sign mechanism
if ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} $(gen_ed25519_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} $(gen_ed25519_random_public) $(gen_ed25519_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC} $(gen_ed25519_random_public) $(gen_ed25519_random_public)
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} $(gen_ed25519_random_public) $(gen_ed25519_random_public) ${PUBLIC} $(gen_ed25519_random_public) $(gen_ed25519_random_public)
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
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 ${COMMIT} ${PUBLIC} >out.txt
assert_file_has_content out.txt "ed25519: Signature verified successfully with key"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN} >out.txt
assert_file_has_content out.txt "dummy: Signature verified"
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
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT} >out.txt
assert_file_has_content out.txt 'ed25519: Signature verified successfully'

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_ed25519_random_public
done > ${PUBKEYS}
# Check if file contain no valid signatures
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT} 2>err.txt; then
    fatal "validated with no signatures"
fi
assert_file_has_content err.txt 'error:.* ed25519: Signature couldn.t be verified; tried 100 keys'
# Check if no valid signatures provided via args&file
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi

#Test keys file and public key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-file=${PUBKEYS} ${COMMIT} ${PUBLIC}

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

# Check the well-known places mechanism
mkdir -p ${test_tmpdir}/{trusted,revoked}.ed25519.d
for((i=0;i<100;i++)); do
    # Generate some key files with random public signatures
    gen_ed25519_random_public > ${test_tmpdir}/trusted.ed25519.d/signature_$i
done
# Check no valid public keys are available
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
echo ${PUBLIC} > ${test_tmpdir}/trusted.ed25519.d/correct
# Verify with correct key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-dir=${test_tmpdir} ${COMMIT}

echo "ok verify ed25519 system-wide configuration"

# Add the public key into revoked list
echo ${PUBLIC} > ${test_tmpdir}/revoked.ed25519.d/correct
# Check if public key is not valid anymore
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=ed25519 --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
rm -rf ${test_tmpdir}/{trusted,revoked}.ed25519.d
echo "ok verify ed25519 revoking keys mechanism"
