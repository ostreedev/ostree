#!/bin/bash
#
# Copyright (C) 2019 Endless Mobile, Inc.
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

setup_fake_remote_repo1 "archive" "--generate-sizes"

echo '1..3'

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

# Pull commit metadata only. All size and objects will be needed.
${CMD_PREFIX} ostree --repo=repo pull --commit-metadata-only origin main
${CMD_PREFIX} ostree --repo=repo show --print-sizes origin:main > show.txt
assert_file_has_content show.txt 'Compressed size (needed/total): 637[  ]bytes/637[  ]bytes'
assert_file_has_content show.txt 'Unpacked size (needed/total): 457[  ]bytes/457[  ]bytes'
assert_file_has_content show.txt 'Number of objects (needed/total): 10/10'
echo "ok sizes commit metadata only"

# Pull the parent commit so we get most of the objects
parent=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main^)
${CMD_PREFIX} ostree --repo=repo pull origin ${parent}
${CMD_PREFIX} ostree --repo=repo show --print-sizes origin:main > show.txt
assert_file_has_content show.txt 'Compressed size (needed/total): 501[  ]bytes/637[  ]bytes'
assert_file_has_content show.txt 'Unpacked size (needed/total): 429[  ]bytes/457[  ]bytes'
assert_file_has_content show.txt 'Number of objects (needed/total): 6/10'
echo "ok sizes commit partial"

# Finish pulling the commit and check that no objects needed
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo show --print-sizes origin:main > show.txt
assert_file_has_content show.txt 'Compressed size (needed/total): 0[  ]bytes/637[  ]bytes'
assert_file_has_content show.txt 'Unpacked size (needed/total): 0[  ]bytes/457[  ]bytes'
assert_file_has_content show.txt 'Number of objects (needed/total): 0/10'
echo "ok sizes commit full"
