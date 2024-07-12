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

echo "Unsigned commit" > file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Unsigned commit'
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"

# Test `ostree sign` with dummy module first
DUMMYSIGN="dummysign"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy ${COMMIT} ${DUMMYSIGN}

# Ensure that detached metadata really contain expected string
EXPECTEDSIGN="$(echo $DUMMYSIGN | hexdump -n 9 -e '8/1 "0x%.2x, " 1/1 " 0x%.2x"')"
${CMD_PREFIX} ostree --repo=repo show ${COMMIT} --print-detached-metadata-key=ostree.sign.dummy | grep -q -e "${EXPECTEDSIGN}"
tap_ok "Detached dummy signature added"

# Verify vith sign mechanism
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN}
tap_ok "dummy signature verified"

echo "Signed commit with dummy key: ${DUMMYSIGN}" >> file.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo commit -b main -s 'Signed with dummy module' --sign=${DUMMYSIGN} --sign-type=dummy 
COMMIT="$(ostree --repo=${test_tmpdir}/repo rev-parse main)"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN}
tap_ok "commit with dummy signing"

if ${CMD_PREFIX} env -u OSTREE_DUMMY_SIGN_ENABLED ostree --repo=${test_tmpdir}/repo sign --sign-type=dummy --verify ${COMMIT} ${DUMMYSIGN} 2>err.txt; then
    fatal "verified dummy signature without env"
fi
# FIXME the error message here is broken
#assert_file_has_content_literal err.txt 'dummy signature type is only for ostree testing'
assert_file_has_content_literal err.txt ' No valid signatures found'
tap_ok "dummy sig requires env"

tap_end
