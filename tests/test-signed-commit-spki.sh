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

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1

mkdir ${test_tmpdir}/repo
ostree_repo_init repo --mode="archive"

# For multi-sign test
DUMMYSIGN="dummysign"

# Test ostree sign with 'spki' module
gen_spki_keys
PUBLIC=${SPKIPUBLIC}
SECRET=${SPKISECRET}

WRONG_PUBLIC="$(gen_spki_random_public)"

echo "PUBLIC = $PUBLIC"

echo "Signed commit with spki: ${SECRET}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s "Signed with spki module" --sign="${SECRET}" --sign-type=spki
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Ensure that detached metadata contain signature
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.spki &>/dev/null
tap_ok "Detached spki signature added"

# Verify vith sign mechanism
if ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} ${PUBLIC} ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} $(gen_spki_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} $(gen_spki_random_public) $(gen_spki_random_public) ${PUBLIC}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} ${PUBLIC} $(gen_spki_random_public) $(gen_spki_random_public)
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} $(gen_spki_random_public) $(gen_spki_random_public) ${PUBLIC} $(gen_spki_random_public) $(gen_spki_random_public)
tap_ok "spki signature verified"

# Check if we are able to use all available modules to sign the same commit
echo "Unsigned commit for multi-sign" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"
# Check if we have no signatures
for mod in "dummy" "spki"; do
    if ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.${mod}; then
        echo "Unexpected signature for ${mod} found"
        exit 1
    fi
done

# Sign with all available modules
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy ${COMMIT} ${DUMMYSIGN}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=spki ${COMMIT} ${SECRET}
# and verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki ${COMMIT} ${PUBLIC} >out.txt
assert_file_has_content out.txt "spki: Signature verified successfully with key"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN} >out.txt
assert_file_has_content out.txt "dummy: Signature verified"
tap_ok "multiple signing "

# Prepare files with public spki signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} spki_XXXXXX.spki)"

# Test if file contain no keys
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT}; then
    exit 1
fi

# Test if have a problem with file object
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${test_tmpdir} ${COMMIT}; then
    exit 1
fi

# Test with single key in list
cat ${SPKIPUBLICPEM} > ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT} >out.txt
assert_file_has_content out.txt 'spki: Signature verified successfully'

# Test the file with multiple keys without a valid public key
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_spki_random_public_pem
done > ${PUBKEYS}
# Check if file contain no valid signatures
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT} 2>err.txt; then
    fatal "validated with no signatures"
fi
assert_file_has_content err.txt 'error:.* spki: Signature couldn.t be verified; tried 100 keys'
# Check if no valid signatures provided via args&file
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT} ${WRONG_PUBLIC}; then
    exit 1
fi

#Test keys file and public key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT} ${PUBLIC}

# Add correct key into the list
cat "${SPKIPUBLICPEM}" >> ${PUBKEYS}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT}

tap_ok "verify spki keys file"

# Check spki signing with secret file
echo "Unsigned commit for secret file usage" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

KEYFILE="$(mktemp -p ${test_tmpdir} secret_XXXXXX.spki)"
cat "${SPKISECRETPEM}" > ${KEYFILE}
# Sign
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=spki --keys-file=${KEYFILE} ${COMMIT}
# Verify
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-file=${PUBKEYS} ${COMMIT}
tap_ok "sign with spki keys file"

# Check the well-known places mechanism
mkdir -p ${test_tmpdir}/{trusted,revoked}.spki.d
for((i=0;i<100;i++)); do
    # Generate some key files with random public signatures
    gen_spki_random_public_pem > ${test_tmpdir}/trusted.spki.d/signature_$i
done
# Check no valid public keys are available
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
cat "${SPKIPUBLICPEM}" > ${test_tmpdir}/trusted.spki.d/correct
# Verify with correct key
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-dir=${test_tmpdir} ${COMMIT}

tap_ok "verify spki system-wide configuration"

# Add the public key into revoked list
cat "${SPKIPUBLICPEM}" > ${test_tmpdir}/revoked.spki.d/correct
# Check if public key is not valid anymore
if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --verify --sign-type=spki --keys-dir=${test_tmpdir} ${COMMIT}; then
    exit 1
fi
rm -rf ${test_tmpdir}/{trusted,revoked}.spki.d
tap_ok "verify spki revoking keys mechanism"

tap_end
