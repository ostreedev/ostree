#!/bin/bash
#
# Copyright (C) 2015 Red Hat, Inc.
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

setup_fake_remote_repo1 "archive-z2"

cd ${test_tmpdir}
mkdir repo
${OSTREE} init

#----------------------------------------------
# Test synchronicity of keyring file and remote
#----------------------------------------------

assert_not_has_file repo/R1.trustedkeys.gpg

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

assert_not_has_file repo/R1.trustedkeys.gpg

# Import one valid key ID
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

assert_has_file repo/R1.trustedkeys.gpg

${OSTREE} remote delete R1

assert_not_has_file repo/R1.trustedkeys.gpg

#---------------------------------------
# Test gpg-import with --keyring option
#---------------------------------------

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import one valid key ID
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

# Import multiple valid key IDs
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 2 GPG key'

# Import key IDs we already have, make sure they're caught
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} ${TEST_GPG_KEYID_3} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 0 GPG key'

${OSTREE} remote delete R1

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import all keys from keyring
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 3 GPG key'

${OSTREE} remote delete R1

#-------------------------------------
# Test gpg-import with --stdin option
#-------------------------------------

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import ASCII-armored keys thru stdin
cat ${test_tmpdir}/gpghome/key{1,2,3}.asc | ${OSTREE} remote gpg-import --stdin R1 | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 3 GPG key'

${OSTREE} remote delete R1

#------------------------------------------------------------
# Divide keys across multiple remotes, test GPG verification
# For testing purposes the remotes all point to the same URL
# This also tests "remote add" with --gpg-import.
#------------------------------------------------------------

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc R1 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key2.asc R2 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key3.asc R3 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

# Checkout the "remote" repo so we can add more commits
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main workdir

# Sign a new commit with key1 and try pulling from each remote
echo shadow > workdir/blinky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add blinky" --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R2:main >/dev/null 2>&1; then
    assert_not_reached "(key1/R2) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R3:main >/dev/null 2>&1; then
    assert_not_reached "(key1/R3) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R1:main >/dev/null

# Sign a new commit with key2 and try pulling from each remote
echo speedy > workdir/pinky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add pinky" --gpg-sign ${TEST_GPG_KEYID_2} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R1:main >/dev/null 2>&1; then
    assert_not_reached "(key2/R1) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R3:main >/dev/null 2>&1; then
    assert_not_reached "(key2/R3) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R2:main >/dev/null

# Sign a new commit with key3 and try pulling from each remote
echo bashful > workdir/inky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add inky" --gpg-sign ${TEST_GPG_KEYID_3} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R1:main >/dev/null 2>&1; then
    assert_not_reached "(key3/R1) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R2:main >/dev/null 2>&1; then
    assert_not_reached "(key3/R2) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R3:main >/dev/null
