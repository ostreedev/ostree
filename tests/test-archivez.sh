#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

echo '1..11'

setup_test_repository "archive-z2"

. ${test_srcdir}/archive-test.sh

cd ${test_tmpdir}
mkdir repo2
${CMD_PREFIX} ostree --repo=repo2 init
${CMD_PREFIX} ostree --repo=repo2 remote add --set=gpg-verify=false aremote file://$(pwd)/repo test2
${CMD_PREFIX} ostree --repo=repo2 pull aremote
${CMD_PREFIX} ostree --repo=repo2 rev-parse aremote/test2
${CMD_PREFIX} ostree --repo=repo2 fsck
echo "ok pull with from file:/// uri"
