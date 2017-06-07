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

setup_test_repository "bare-user-only"
extra_basic_tests=1
. $(dirname $0)/basic-test.sh

# Reset things so we don't inherit a lot of state from earlier tests
cd ${test_tmpdir}
rm repo files -rf
ostree_repo_init repo init --mode=bare-user-only

# Init an archive repo where we'll store content that can't go into bare-user
cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive
cd ${test_tmpdir}
cat > statoverride.txt <<EOF
2048 /some-setuid
EOF
mkdir -p files/
echo "a setuid file" > files/some-setuid
chmod 0644 files/some-setuid
$CMD_PREFIX ostree --repo=repo-input commit -b content-with-suid --statoverride=statoverride.txt --tree=dir=files
if $CMD_PREFIX ostree pull-local --repo=repo repo-input 2>err.txt; then
    assert_not_reached "copying suid file into bare-user worked?"
fi
assert_file_has_content err.txt "Invalid mode.*with bits 040.*in bare-user-only"
echo "ok failed to commit suid"
