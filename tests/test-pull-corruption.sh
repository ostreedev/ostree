#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

set -e

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive-z2"

echo '1..1'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

do_corrupt_pull_test() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ${CMD_PREFIX} ostree --repo=repo init
    ${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
    if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
	assert_not_reached "pull unexpectedly succeeded!"
    fi
    rm -rf ${repopath}
    cp -a ${repopath}.orig ${repopath}
}

# Corrupt .dirmeta
someobject=$(find ${repopath} -name '*.dirmeta' | head -1)
echo "garbage garbage garbage" > ${someobject}
do_corrupt_pull_test
echo "ok corrupt dirmeta"

# Corrupt .dirtree
someobject=$(find ${repopath} -name '*.dirtree' | head -1)
echo "garbage garbage garbage" > ${someobject}
do_corrupt_pull_test
echo "ok corrupt dirtree"

# Corrupt .filez
someobject=$(find ${repopath} -name '*.filez' | head -1)
echo "garbage garbage garbage" > ${someobject}
do_corrupt_pull_test
echo "ok corrupt filez"
