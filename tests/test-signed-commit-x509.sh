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

echo "1..7"

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1

mkdir ${test_tmpdir}/repo
ostree_repo_init repo --mode="archive"

# For multi-sign test
DUMMYSIGN="dummysign"

# Test ostree sign with 'x509' module
gen_x509_keys
PUBLIC=${X509PUBLIC}
SECRET=${X509SECRET}

WRONG_PUBLIC="$(gen_x509_random_public)"

echo "PUBLIC = $PUBLIC"

echo "Signed commit with x509: ${SECRET}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s "Signed with x509 module" --sign="${SECRET}" --sign-type=x509
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Ensure that detached metadata contain signature
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.x509 &>/dev/null
echo "ok Detached x509 signature added"

# Verify vith sign mechanism
if ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} ${PUBLIC} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} $(gen_x509_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} $(gen_x509_random_public) $(gen_x509_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} ${PUBLIC} $(gen_x509_random_public) $(gen_x509_random_public)
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} $(gen_x509_random_public) $(gen_x509_random_public) ${PUBLIC} $(gen_x509_random_public) $(gen_x509_random_public)
echo "ok x509 signature verified"

# Check if we able to use all available modules to sign the same commit
echo "Unsigned commit for multi-sign" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"
# Check if we have no signatures
for mod in "dummy" "x509"; do
    if ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.${mod}; then
        echo "Unexpected signature for ${mod} found"
        exit 1
    fi
done

# Sign with all available modules
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy ${COMMIT} ${DUMMYSIGN}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=x509 ${COMMIT} ${SECRET}
# and verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 ${COMMIT} ${PUBLIC} >out.txt
assert_file_has_content out.txt "x509: Signature verified successfully with key"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN} >out.txt
assert_file_has_content out.txt "dummy: Signature verified"
echo "ok multiple signing "

# Prepare files with public x509 signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} x509_XXXXXX.x509)"

# Test if file contain no keys
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT}; then
    exit 1
fi

# Test if have a problem with file object
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${test_tmpdir} ${COMMIT}; then
    exit 1
fi

# Test with single key in list
echo ${PUBLIC} > ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT} >out.txt
assert_file_has_content out.txt 'x509: Signature verified successfully'

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_x509_random_public
done > ${PUBKEYS}
# Check if file contain no valid signatures
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT} 2>err.txt; then
    fatal "validated with no signatures"
fi
assert_file_has_content err.txt 'error:.* x509: Signature couldn.t be verified; tried 100 keys'
# Check if no valid signatures provided via args&file
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi

#Test keys file and public key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT} ${PUBLIC}

# Add correct key into the list
echo ${PUBLIC} >> ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT}

echo "ok verify x509 keys file"

# Check x509 signing with secret file
echo "Unsigned commit for secret file usage" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

KEYFILE="$(mktemp -p ${test_tmpdir} secret_XXXXXX.x509)"
echo "${SECRET}" > ${KEYFILE}
# Sign
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=x509 --keys-file=${KEYFILE} ${COMMIT}
# Verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-file=${PUBKEYS} ${COMMIT}
echo "ok sign with x509 keys file"

# Check the well-known places mechanism
mkdir -p ${test_tmpdir}/{trusted,revoked}.x509.d
for((i=0;i<100;i++)); do
    # Generate some key files with random public signatures
    gen_x509_random_public > ${test_tmpdir}/trusted.x509.d/signature_$i
done
# Check no valid public keys are available
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
echo ${PUBLIC} > ${test_tmpdir}/trusted.x509.d/correct
# Verify with correct key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-dir=${test_tmpdir} ${COMMIT}

echo "ok verify x509 system-wide configuration"

# Add the public key into revoked list
echo ${PUBLIC} > ${test_tmpdir}/revoked.x509.d/correct
# Check if public key is not valid anymore
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=x509 --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
rm -rf ${test_tmpdir}/{trusted,revoked}.x509.d
echo "ok verify x509 revoking keys mechanism"
