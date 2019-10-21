#!/bin/bash
#
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

# We don't want OSTREE_GPG_HOME used for these tests.
unset OSTREE_GPG_HOME

setup_fake_remote_repo1 "archive"

# Use the local http server for GPG key update tests
_OSTREE_GPG_UPDATE_LOCAL_PORT=$(cat ${test_tmpdir}/httpd-port)
export _OSTREE_GPG_UPDATE_LOCAL_PORT

echo "1..6"

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo

# Check that update-gpg-keys works with no existing keys
${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} remote update-gpg-keys R1 > result
assert_file_empty result
echo "ok remote with no gpg keys"

# Import a GPG key and check that no updates found
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/key1.asc R1
${OSTREE} remote update-gpg-keys R1 > result
assert_file_empty result
echo "ok update no keys found"

# Test advanced update URL
rm -rf ${test_tmpdir}/httpd/.well-known/openpgpkey/
mkdir -p ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/
cp ${test_tmpdir}/gpghome/trusted/pubring.gpg \
  ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u
${OSTREE} remote update-gpg-keys R1 > result
assert_file_has_content result 'Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA'
assert_file_has_content result 'UID: Ostree Tester <test@test.com>'
assert_not_file_has_content result 'Key: 7B3B1020D74479687FDB2273D8228CFECA950D41'
assert_not_file_has_content result 'UID: Ostree Tester II <test2@test.com>'
assert_not_file_has_content result 'Key: 7D29CF060B8269CDF63BFBDD0D15FAE7DF444D67'
assert_not_file_has_content result 'UID: Ostree Tester III <test3@test.com>'
echo "ok update advanced URL"

# Test direct update URL
rm -rf ${test_tmpdir}/httpd/.well-known/openpgpkey/
mkdir -p ${test_tmpdir}/httpd/.well-known/openpgpkey/hu/
cp ${test_tmpdir}/gpghome/trusted/pubring.gpg \
  ${test_tmpdir}/httpd/.well-known/openpgpkey/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u
${OSTREE} remote update-gpg-keys R1 > result
assert_file_has_content result 'Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA'
assert_file_has_content result 'UID: Ostree Tester <test@test.com>'
assert_not_file_has_content result 'Key: 7B3B1020D74479687FDB2273D8228CFECA950D41'
assert_not_file_has_content result 'UID: Ostree Tester II <test2@test.com>'
assert_not_file_has_content result 'Key: 7D29CF060B8269CDF63BFBDD0D15FAE7DF444D67'
assert_not_file_has_content result 'UID: Ostree Tester III <test3@test.com>'
echo "ok update direct URL"

# Test invalid remote GPG key
rm -rf ${test_tmpdir}/httpd/.well-known/openpgpkey/
mkdir -p ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/
echo invalid > ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u
${OSTREE} --verbose remote update-gpg-keys R1 > result
assert_file_empty result
echo "ok ignored invalid remote GPG key"

# Test importing a revoked subkey
rm -rf ${test_tmpdir}/httpd/.well-known/openpgpkey/
mkdir -p ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/
cp ${test_tmpdir}/gpghome/key1-subkey-revoked.asc \
  ${test_tmpdir}/httpd/.well-known/openpgpkey/test.com/hu/iffe93qcsgp4c8ncbb378rxjo6cn9q6u
${OSTREE} --verbose remote update-gpg-keys R1 > result
assert_file_has_content result 'Key: 5E65DE75AB1C501862D476347FCA23D8472CDAFA'
assert_file_has_content result 'UID: Ostree Tester <test@test.com>'
assert_file_has_content result 'Subkey: CC47B2DFB520AEF231180725DF20F58B408DEA49 (revoked)'
echo "ok update revoked subkey"
