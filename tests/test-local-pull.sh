#!/bin/bash
#
# Copyright (C) 2014 Alexander Larsson <alexl@redhat.com>
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

echo "1..1"

. $(dirname $0)/libtest.sh

setup_test_repository "archive-z2"
echo "ok setup"

cd ${test_tmpdir}
mkdir repo2
${CMD_PREFIX} ostree --repo=repo2 init --mode="bare-user"

${CMD_PREFIX} ostree --repo=repo2 pull-local repo
${CMD_PREFIX} ostree --repo=repo2 fsck
echo "ok pull-local z2 to bare-user"

mkdir repo3
${CMD_PREFIX} ostree --repo=repo3 init --mode="archive-z2"
${CMD_PREFIX} ostree --repo=repo3 pull-local repo2
${CMD_PREFIX} ostree --repo=repo3 fsck
echo "ok pull-local bare-user to z2"


# Verify the name + size + mode + type + symlink target + owner/group are the same
# for all checkouts
${CMD_PREFIX} ostree checkout --repo repo test2 checkout1
find checkout1 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout1.files

${CMD_PREFIX} ostree checkout --repo repo2 test2 checkout2
find checkout2 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout2.files

${CMD_PREFIX} ostree checkout --repo repo3 test2 checkout3
find checkout3 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout3.files

cmp checkout1.files checkout2.files
cmp checkout1.files checkout3.files
echo "ok checkouts same"
