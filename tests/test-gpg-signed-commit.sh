#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
# Copyright (C) 2015 Red Hat, Inc.
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

if ! has_gpgme; then
    echo "1..0 #SKIP no gpgme support compiled in"
    exit 0
fi

num_tests=1

# Run some more tests if an appropriate GPG is available
num_gpg_tests=8
GPG=$(which_gpg)
if [ -n "${GPG}" ]; then
  let num_tests+=num_gpg_tests
fi

echo "1..${num_tests}"

setup_test_repository "archive"

export OSTREE_GPG_SIGN="${OSTREE} gpg-sign --gpg-homedir=${TEST_GPG_KEYHOME}"

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
# We at least got some content here and ran through the code; later
# tests will actually do verification
assert_file_has_content test2-show 'Found 1 signature'

${OSTREE} show --gpg-homedir=${TEST_GPG_KEYHOME} test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 1 signature'

# Now sign a commit with 3 different keys
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID_1} --gpg-sign=${TEST_GPG_KEYID_2} --gpg-sign=${TEST_GPG_KEYID_3} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 3 signature'

# Commit and sign separately, then monkey around with signatures
cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --tree=dir=files
if ${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature'; then
  assert_not_reached
fi
${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_1}
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 1 signature'
# Signing with a previously used key should be caught
if ${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_1} 2>/dev/null; then
  assert_not_reached
fi
# Add a few more signatures and then delete them
${OSTREE_GPG_SIGN} test2 ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3}
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 3 signature'
${OSTREE_GPG_SIGN} --delete test2 ${TEST_GPG_KEYID_2} | grep -o 'Signatures deleted: [[:digit:]]' > test2-delete
assert_file_has_content test2-delete 'Signatures deleted: 1'
${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature' > test2-show
assert_file_has_content test2-show 'Found 2 signature'
# Already deleted TEST_GPG_KEYID_2; should be ignored
${OSTREE_GPG_SIGN} --delete test2 ${TEST_GPG_KEYID_1} ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3} | grep -o 'Signatures deleted: [[:digit:]]' > test2-delete
assert_file_has_content test2-delete 'Signatures deleted: 2'
# Verify all signatures are gone
if ${OSTREE} show test2 | grep -o 'Found [[:digit:]] signature'; then
  assert_not_reached
fi

echo "ok"

# Remaining tests require gpg
if [ -z "${GPG}" ]; then
  exit 0
fi

# Although we have the gpg --set-expire option, we want to use it to set
# the expiration date for subkeys below. That was only added in gnupg
# 2.1.22. Check if SUBKEY-FPRS is in the usage output.
# (https://git.gnupg.org/cgi-bin/gitweb.cgi?p=gnupg.git;a=blob;f=NEWS;hb=HEAD)
if (${GPG} --quick-set-expire 2>&1 || :) | grep -q SUBKEY-FPRS; then
  GPG_CAN_EXPIRE_SUBKEYS=true
else
  GPG_CAN_EXPIRE_SUBKEYS=false
fi

# Create a temporary GPG homedir
tmpgpg_home=${test_tmpdir}/tmpgpghome
mkdir -m700 ${tmpgpg_home}

# Wire up an exit hook to kill the gpg-agent in it
cleanup_tmpgpg_home() {
  libtest_cleanup_gpg ${tmpgpg_home}
}
libtest_exit_cmds+=(cleanup_tmpgpg_home)

# Create an temporary trusted GPG directory
tmpgpg_trusted=${test_tmpdir}/tmpgpgtrusted
tmpgpg_trusted_keyring=${tmpgpg_trusted}/keyring.gpg
export OSTREE_GPG_HOME=${tmpgpg_trusted}
mkdir -p ${tmpgpg_trusted}

# Create one normal signing key and one signing key with a subkey. See
# https://www.gnupg.org/documentation/manuals/gnupg/Unattended-GPG-key-generation.html.
${GPG} --homedir=${tmpgpg_home} --batch --generate-key <<"EOF"
Key-Type: RSA
Key-Length: 2048
Key-Usage: sign
Name-Real: Test Key 1
Expire-Date: 0
%no-protection
%transient-key
%commit
Key-Type: RSA
Key-Length: 2048
Key-Usage: sign
Subkey-Type: RSA
Subkey-Length: 2048
Subkey-Usage: sign
Name-Real: Test Key 2
Expire-Date: 0
%no-protection
%transient-key
%commit
EOF

# Figure out the key IDs and fingerprints. Assume that the order of the
# keys matches those specified in the generation.
#
# https://git.gnupg.org/cgi-bin/gitweb.cgi?p=gnupg.git;a=blob_plain;f=doc/DETAILS
key1_id=
key1_fpr=
key2_id=
key2_fpr=
key2_sub_id=
key2_sub_fpr=
gpg_seckey_listing=$(${GPG} --homedir=${tmpgpg_home} --list-secret-keys --with-colons)
while IFS=: read -a fields; do
  if [ "${fields[0]}" = sec ]; then
    # Secret key - key ID is in field 5
    if [ -z "${key1_id}" ]; then
      key1_id=${fields[4]}
    else
      key2_id=${fields[4]}
    fi
  elif [ "${fields[0]}" = ssb ]; then
    # Secret subkey - key ID is in field 5
    key2_sub_id=${fields[4]}
  elif [ "${fields[0]}" = fpr ]; then
    # Fingerprint record - the fingerprint ID is in field 10
    if [ -z "${key1_fpr}" ]; then
      key1_fpr=${fields[9]}
    elif [ -z "${key2_fpr}" ]; then
      key2_fpr=${fields[9]}
    else
      key2_sub_fpr=${fields[9]}
    fi
  fi
done <<< "${gpg_seckey_listing}"

# Create a commit and sign it with both key1 and key2_sub
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" \
  --tree=dir=files --gpg-homedir=${tmpgpg_home} \
  --gpg-sign=${key1_id} --gpg-sign=${key2_sub_id}
${OSTREE} show test2 > test2-show
assert_file_has_content test2-show '^Found 2 signatures'
assert_file_has_content test2-show 'public key not found'
assert_file_has_content test2-show "${key1_id}"
assert_file_has_content test2-show "${key2_sub_id}"

echo "ok signed with both generated keys"

# Export the public keys and check again
${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}
${OSTREE} show test2 > test2-show
assert_file_has_content test2-show '^Found 2 signatures'
assert_not_file_has_content test2-show 'public key not found'
assert_file_has_content test2-show "${key1_id}"
assert_file_has_content test2-show "${key2_sub_id}"
assert_file_has_content test2-show 'Good signature from "Test Key 1 <>"'
assert_file_has_content test2-show 'Good signature from "Test Key 2 <>"'
assert_file_has_content test2-show "Primary key ID ${key2_id}"

echo "ok verified both generated keys"

# Make key1 expired, wait until it's expired, export the public keys and check
# again
${GPG} --homedir=${tmpgpg_home} --quick-set-expire ${key1_fpr} seconds=1
sleep 2
${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}
${OSTREE} show test2 > test2-show
assert_file_has_content test2-show '^Found 2 signatures'
assert_not_file_has_content test2-show 'public key not found'
assert_file_has_content test2-show "${key1_id}"
assert_file_has_content test2-show "${key2_sub_id}"
assert_file_has_content test2-show 'BAD signature from "Test Key 1 <>"'
assert_file_has_content test2-show 'Key expired'
assert_file_has_content test2-show 'Good signature from "Test Key 2 <>"'
assert_file_has_content test2-show "Primary key ID ${key2_id}"

echo "ok verified with key1 expired"

# Unexpire key1, expire key2 primary, wait until it's expired, export the
# public keys and check again
${GPG} --homedir=${tmpgpg_home} --quick-set-expire ${key1_fpr} seconds=0
${GPG} --homedir=${tmpgpg_home} --quick-set-expire ${key2_fpr} seconds=1
sleep 2
${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}
${OSTREE} show test2 > test2-show
assert_file_has_content test2-show '^Found 2 signatures'
assert_not_file_has_content test2-show 'public key not found'
assert_file_has_content test2-show "${key1_id}"
assert_file_has_content test2-show "${key2_sub_id}"
assert_file_has_content test2-show 'Good signature from "Test Key 1 <>"'
assert_file_has_content test2-show 'BAD signature from "Test Key 2 <>"'
assert_file_has_content test2-show "Primary key ID ${key2_id}"
assert_file_has_content test2-show 'Primary key expired'

echo "ok verified with key2 primary expired"

# If subkey expiration is available, expire key2 subkey, wait until it's
# expired, export the public keys and check again
if ${GPG_CAN_EXPIRE_SUBKEYS}; then
  ${GPG} --homedir=${tmpgpg_home} --quick-set-expire ${key2_fpr} seconds=1 ${key2_sub_fpr}
  sleep 2
  ${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}
  ${OSTREE} show test2 > test2-show
  assert_file_has_content test2-show '^Found 2 signatures'
  assert_not_file_has_content test2-show 'public key not found'
  assert_file_has_content test2-show "${key1_id}"
  assert_file_has_content test2-show "${key2_sub_id}"
  assert_file_has_content test2-show 'Good signature from "Test Key 1 <>"'
  assert_file_has_content test2-show 'BAD signature from "Test Key 2 <>"'
  assert_file_has_content test2-show 'Key expired'
  assert_file_has_content test2-show "Primary key ID ${key2_id}"
  assert_file_has_content test2-show 'Primary key expired'

  echo "ok verified with key2 primary and subkey expired"
else
  echo "ok # SKIP gpg --quick-set-expire does not support expiring subkeys"
fi

# Unexpire key2 primary and export the public keys
${GPG} --homedir=${tmpgpg_home} --quick-set-expire ${key2_fpr} seconds=0
${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}

# This test expects the subkey to be expired, so skip it if that didn't happen
if ${GPG_CAN_EXPIRE_SUBKEYS}; then
  ${OSTREE} show test2 > test2-show
  assert_file_has_content test2-show '^Found 2 signatures'
  assert_not_file_has_content test2-show 'public key not found'
  assert_file_has_content test2-show "${key1_id}"
  assert_file_has_content test2-show "${key2_sub_id}"
  assert_file_has_content test2-show 'Good signature from "Test Key 1 <>"'
  assert_file_has_content test2-show 'BAD signature from "Test Key 2 <>"'
  assert_file_has_content test2-show 'Key expired'
  assert_file_has_content test2-show "Primary key ID ${key2_id}"
  assert_not_file_has_content test2-show 'Primary key expired'

  echo "ok verified with key2 subkey expired"
else
  echo "ok # SKIP gpg --quick-set-expire does not support expiring subkeys"
fi

# Add a second subkey but don't export it to the trusted keyring so that a new
# commit signed with fails.
${GPG} --homedir=${tmpgpg_home} --batch --passphrase '' \
  --quick-add-key ${key2_fpr} rsa2048 sign never
gpg_seckey_listing=$(${GPG} --homedir=${tmpgpg_home} --list-secret-keys --with-colons)
key2_sub2_id=$(awk -F: '{if ($1 == "ssb") print $5}' <<< "${gpg_seckey_listing}" | tail -n1)
key2_sub2_fpr=$(awk -F: '{if ($1 == "fpr") print $10}' <<< "${gpg_seckey_listing}" | tail -n1)
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" \
  --tree=dir=files --gpg-homedir=${tmpgpg_home} \
  --gpg-sign=${key1_id} --gpg-sign=${key2_sub2_id}
${OSTREE} show test2 > test2-show
assert_file_has_content test2-show '^Found 2 signatures'
assert_file_has_content test2-show "${key1_id}"
assert_file_has_content test2-show "${key2_sub2_id}"
assert_file_has_content test2-show 'Good signature from "Test Key 1 <>"'
assert_file_has_content test2-show 'public key not found'

echo "ok verified with key2 sub2 missing"

# Revoke key1 by importing the revocation certificate created when generating
# the key. The cert should be in $homedir/openpgp-revocs.d/$fpr.rev with
# recent gnupg. However, it tries to prevent you from accidentally revoking
# things by adding a : before the block, so it needs to be stripped.
key1_rev=${tmpgpg_home}/openpgp-revocs.d/${key1_fpr}.rev
if [ -f ${key1_rev} ]; then
  sed -i '/BEGIN PGP PUBLIC KEY BLOCK/s/^://' ${key1_rev}
  ${GPG} --homedir=${tmpgpg_home} --import ${key1_rev}
  # Export both keys again
  ${GPG} --homedir=${tmpgpg_home} --export ${key1_id} ${key2_id} > ${tmpgpg_trusted_keyring}
  ${OSTREE} show test2 > test2-show
  assert_file_has_content test2-show '^Found 2 signatures'
  assert_not_file_has_content test2-show 'public key not found'
  assert_file_has_content test2-show "${key1_id}"
  assert_file_has_content test2-show "${key2_sub2_id}"
  assert_file_has_content test2-show 'Key revoked'
  assert_file_has_content test2-show 'Good signature from "Test Key 2 <>"'
  assert_file_has_content test2-show "Primary key ID ${key2_id}"

  echo "ok verified with key1 revoked"
else
  echo "ok # SKIP could not find key revocation certificate"
fi
