#!/bin/bash
#
# Copyright (C) 2016 Alexander Larsson <alexl@redhat.com>
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

skip_without_user_xattrs

echo '1..2'

setup_test_repository "archive-z2"

export OSTREE_GPG_SIGN="${OSTREE} gpg-sign --gpg-homedir=${TEST_GPG_KEYHOME}"

cd ${test_tmpdir}

# Create a repo
${CMD_PREFIX} ostree --repo=repo2 init
${CMD_PREFIX} ostree --repo=repo2 remote add --gpg-import=${test_tmpdir}/gpghome/trusted/pubring.gpg --set=gpg-verify=true aremote file://$(pwd)/repo test2

# Create a repo with repo2 as parent
${CMD_PREFIX} ostree init --repo=repo3 --mode=bare-user
${CMD_PREFIX} ostree config --repo=repo3 set core.parent `pwd`/repo2

# Ensure the unsigned pull fails so we know we imported the gpg config correctly
if ${CMD_PREFIX} ostree --repo=repo3 pull aremote; then
    assert_not_reached "GPG verification unexpectedly succeeded"
fi
echo "ok unsigned pull w/parent"

# Make a signed commit and ensure we can now pull
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} --tree=dir=files
${CMD_PREFIX} ostree --repo=repo3 pull aremote

echo "ok signed pull w/parent"
