#!/bin/bash
#
# Copyright (C) 2016 Red Hat
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

echo "1..1"

COMMIT_SIGN="--gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}"
setup_fake_remote_repo1 "archive-z2" "${COMMIT_SIGN}" --random-500s=50

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo init --mode=archive-z2
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
for x in $(seq 200); do
    if ${CMD_PREFIX} ostree --repo=repo pull --mirror origin main 2>err.txt; then
	echo "Success on iteration ${x}"
	break;
    fi
    assert_file_has_content err.txt "500.*Internal Server Error"
done

${CMD_PREFIX} ostree --repo=repo fsck
${CMD_PREFIX} ostree --repo=repo rev-parse main

echo "ok repeated pull after 500s"
