#!/bin/bash
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

#!/bin/bash
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

# Dead code below
skip_without_user_xattrs

setup_test_repository "archive"

# Generate a commit with a lot of user. xattrs to exercise
# canonicalization (ref https://github.com/ostreedev/ostree/pull/3346/commits/1858d3d300bf12907a13335f616654f422cdaa58)
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo checkout test2 test2-checkout1
echo userxattr-test > test2-checkout1/test-user-xattr-file
for v in $(seq 100); do
    setfattr -n user.$v -v testvalue test2-checkout1/test-user-xattr-file
done
${CMD_PREFIX} ostree --repo=repo commit --canonical-permissions -b test2 --consume --tree=dir=test2-checkout1
rm test2-checkout1 -rf

${CMD_PREFIX} ostree --repo=repo fsck

tap_ok fsck user xattrs

# Now, if we have a /usr/bin/ostree, we hope that it's an old version so
# we cross-check compatibility
if test -x /usr/bin/ostree; then
    # Also explicitly unset LD_LIBRARY_PATH to be sure we're really
    # using the system libostree.so.
    env -u LD_LIBRARY_PATH /usr/bin/ostree --repo=repo fsck
    tap_ok fsck compat
fi

tap_end
