#!/bin/bash
#
# Copyright © 2021 Endless OS Foundation LLC
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

# We don't want OSTREE_GPG_HOME used for most of these tests.
emptydir=${test_tmpdir}/empty
trusteddir=${OSTREE_GPG_HOME}
mkdir ${emptydir}
OSTREE_GPG_HOME=${emptydir}

# Key listings show dates using the local timezone, so specify UTC for
# consistency.
export TZ=UTC

# Some tests require an appropriate gpg
num_non_gpg_tests=5
num_gpg_tests=2
num_tests=$((num_non_gpg_tests + num_gpg_tests))

echo "1..${num_tests}"

setup_test_repository "archive"

cd ${test_tmpdir}
${OSTREE} remote add R1 http://example.com/repo

# No remote keyring should list no keys.
${OSTREE} remote gpg-list-keys R1 > result
assert_file_empty result

echo "ok remote no keyring"

# Make the global keyring available and make sure there are still no
# keys found for a specified remote.
OSTREE_GPG_HOME=${trusteddir}
${OSTREE} remote gpg-list-keys R1 > result
OSTREE_GPG_HOME=${emptydir}
assert_file_empty result

echo "ok remote with global keyring"

# Import a key and check that it's listed
${OSTREE} remote gpg-import --keyring ${TEST_GPG_KEYHOME}/key1.asc R1
${OSTREE} remote gpg-list-keys R1 > result
cat > expected <<"EOF"
Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA
  Created: Tue Sep 10 02:29:42 2013
  UID: Ostree Tester <test@test.com>
  Advanced update URL: https://openpgpkey.test.com/.well-known/openpgpkey/test.com/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u?l=test
  Direct update URL: https://test.com/.well-known/openpgpkey/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u?l=test
  Subkey: CC47B2DFB520AEF231180725DF20F58B408DEA49
    Created: Tue Sep 10 02:29:42 2013
EOF
assert_files_equal result expected

echo "ok remote with keyring"

# Check the global keys with no keyring
OSTREE_GPG_HOME=${emptydir}
${OSTREE} remote gpg-list-keys > result
assert_file_empty result

echo "ok global no keyring"

# Now check the global keys with a keyring
OSTREE_GPG_HOME=${trusteddir}
${OSTREE} remote gpg-list-keys > result
OSTREE_GPG_HOME=${emptydir}
cat > expected <<"EOF"
Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA
  Created: Tue Sep 10 02:29:42 2013
  UID: Ostree Tester <test@test.com>
  Advanced update URL: https://openpgpkey.test.com/.well-known/openpgpkey/test.com/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u?l=test
  Direct update URL: https://test.com/.well-known/openpgpkey/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u?l=test
  Subkey: CC47B2DFB520AEF231180725DF20F58B408DEA49
    Created: Tue Sep 10 02:29:42 2013
Key: 7B3B1020D74479687FDB2273D8228CFECA950D41
  Created: Tue Mar 17 14:00:32 2015
  UID: Ostree Tester II <test2@test.com>
  Advanced update URL: https://openpgpkey.test.com/.well-known/openpgpkey/test.com/hu/nnxwsxno46ap6hw7fgphp68j76egpfa9?l=test2
  Direct update URL: https://test.com/.well-known/openpgpkey/hu/nnxwsxno46ap6hw7fgphp68j76egpfa9?l=test2
  Subkey: 1EFA95C06EB1EB91754575E004B69C2560D53993
    Created: Tue Mar 17 14:00:32 2015
Key: 7D29CF060B8269CDF63BFBDD0D15FAE7DF444D67
  Created: Tue Mar 17 14:01:05 2015
  UID: Ostree Tester III <test3@test.com>
  Advanced update URL: https://openpgpkey.test.com/.well-known/openpgpkey/test.com/hu/8494gyqhmrcs6gn38tn6kgjexet117cj?l=test3
  Direct update URL: https://test.com/.well-known/openpgpkey/hu/8494gyqhmrcs6gn38tn6kgjexet117cj?l=test3
  Subkey: 0E45E48CBF7B360C0E04443E0C601A7402416340
    Created: Tue Mar 17 14:01:05 2015
EOF
assert_files_equal result expected

echo "ok global with keyring"

# Tests checking for expiration and revocation listings require gpg.
GPG=$(which_gpg)
if [ -z "${GPG}" ]; then
    # Print a skip message per skipped test
    for (( i = 0; i < num_gpg_tests; i++ )); do
        echo "ok # SKIP this test requires gpg"
    done
else
    # The GPG private keyring in gpghome is in the older secring.gpg
    # format, but we're likely using a newer gpg. Normally it's
    # implicitly migrated to the newer format, but this test hasn't
    # signed anything, so the private keys haven't been loaded. Force
    # the migration by listing the private keys.
    ${GPG} --homedir=${test_tmpdir}/gpghome -K >/dev/null

    # Expire key1, wait for it to be expired and re-import it.
    ${GPG} --homedir=${test_tmpdir}/gpghome --quick-set-expire ${TEST_GPG_KEYFPR_1} seconds=1
    sleep 2
    ${GPG} --homedir=${test_tmpdir}/gpghome --armor --export ${TEST_GPG_KEYID_1} > ${test_tmpdir}/key1expired.asc
    ${OSTREE} remote gpg-import --keyring ${test_tmpdir}/key1expired.asc R1
    ${OSTREE} remote gpg-list-keys R1 > result
    assert_file_has_content result "^  Expired:"

    echo "ok remote expired key"

    # Revoke key1 and re-import it.
    ${GPG} --homedir=${TEST_GPG_KEYHOME} --import ${TEST_GPG_KEYHOME}/revocations/key1.rev
    ${GPG} --homedir=${test_tmpdir}/gpghome --armor --export ${TEST_GPG_KEYID_1} > ${test_tmpdir}/key1revoked.asc
    ${OSTREE} remote gpg-import --keyring ${test_tmpdir}/key1revoked.asc R1
    ${OSTREE} remote gpg-list-keys R1 > result
    assert_file_has_content result "^Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA (revoked)"
    assert_file_has_content result "^  UID: Ostree Tester <test@test.com> (revoked)"
    assert_file_has_content result "^  Subkey: CC47B2DFB520AEF231180725DF20F58B408DEA49 (revoked)"

    echo "ok remote revoked key"
fi
