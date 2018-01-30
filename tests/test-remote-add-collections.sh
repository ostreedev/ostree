#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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

echo '1..1'

cd ${test_tmpdir}

# Check that adding a remote with a collection ID results in the ID being in the config.
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add some-remote https://example.com/ --collection-id example-id --gpg-import=${test_tmpdir}/gpghome/key1.asc

assert_file_has_content repo/config "^collection-id=example-id$"

echo "ok remote-add-collections"
