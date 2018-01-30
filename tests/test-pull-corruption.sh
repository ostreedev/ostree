#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

# If gjs is not installed, skip the test
if ! gjs --help >/dev/null 2>&1; then
    echo "1..0 # SKIP no gjs"
    exit 0
fi

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive"

echo '1..3'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

do_corrupt_pull_test() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ostree_repo_init repo
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
    if ${CMD_PREFIX} ostree --repo=repo pull origin main; then
        assert_not_reached "pull unexpectedly succeeded!"
    fi
    rm -rf ${repopath}
    cp -a ${repopath}.orig ${repopath}
    ${CMD_PREFIX} ostree --repo=repo pull origin main
    if ${CMD_PREFIX} ostree --repo=repo fsck; then
        echo "ok pull with correct data worked"
    else
        assert_not_reached "pull with correct data failed!"
    fi
}

# FIXME - ignore errors here since gjs in RHEL7 has the final
# unrooting bug
gjs $(dirname $0)/corrupt-repo-ref.js ${repopath} main || true
assert_file_has_content corrupted-status.txt 'Changed byte'
do_corrupt_pull_test
echo "ok corruption"

if ! skip_one_without_user_xattrs; then
    # Set up a corrupted commit object
    rm ostree-srv httpd repo -rf
    setup_fake_remote_repo1 "archive"
    rev=$(ostree --repo=ostree-srv/gnomerepo rev-parse main)
    corruptrev=$(echo ${rev} hello | sha256sum | cut -f 1 -d ' ')
    assert_not_streq ${rev} ${corruptrev}
    rev_path=ostree-srv/gnomerepo/objects/${rev:0:2}/${rev:2}.commit
    corruptrev_path=ostree-srv/gnomerepo/objects/${corruptrev:0:2}/${corruptrev:2}.commit
    mkdir -p $(dirname ${corruptrev_path})
    mv ${rev_path} ${corruptrev_path}
    echo ${corruptrev} > ostree-srv/gnomerepo/refs/heads/main
    ${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
    if ${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo fsck 2>err.txt; then
        assert_not_reached "fsck with corrupted commit worked?"
    fi
    assert_file_has_content_literal err.txt "Corrupted commit object; checksum expected='${corruptrev}' actual='${rev}'"

    # Do a pull-local; this should succeed since we don't verify checksums
    # for local repos by default.
    rm repo err.txt -rf
    ostree_repo_init repo --mode=bare-user
    ${CMD_PREFIX} ostree --repo=repo pull-local ostree-srv/gnomerepo main

    rm repo err.txt -rf
    ostree_repo_init repo --mode=bare-user
    if ${CMD_PREFIX} ostree --repo=repo pull-local --untrusted ostree-srv/gnomerepo main 2>err.txt; then
        assert_not_reached "pull-local --untrusted worked?"
    fi
    assert_file_has_content_literal err.txt "Corrupted commit object; checksum expected='${corruptrev}' actual='${rev}'"

    rm repo err.txt -rf
    ostree_repo_init repo --mode=bare-user
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
    if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>err.txt; then
        assert_not_reached "pull unexpectedly succeeded!"
    fi
    assert_file_has_content_literal err.txt "Corrupted commit object; checksum expected='${corruptrev}' actual='${rev}'"
    echo "ok pull commit corruption"
fi
