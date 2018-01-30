#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"
# This does:
# - init ostree repo in testos-repo
# - create system files in osdata and commit twice those contents into testos-repo
# - copy osdata to osdata-devel and make another change then commit
# - create sysroot with init-fs and os-init and syslinux
# sysroot has /ostree basics (but no contents), syslinux cfg and empty root dirs

echo "1..3"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"

# Now sysroot/ostree has the objects from the testos-repo (obtained over http
# and kept in remote "testos"), but there is no deployment

# This initial deployment gets kicked off with some kernel arguments
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy"

# Create a new branch which we want to migrate to
os_repository_new_commit 1 1 testos/buildmaster/newbranch
# bootcsum now refers to this new commit

# Create a new commit with an empty tree, which marks the original branch as
# EOL, redirecting to the new one.
mkdir empty
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --tree=dir=$(pwd)/empty  --add-metadata-string "ostree.endoflife=Product discontinued" --add-metadata-string "ostree.endoflife-rebase=testos/buildmaster/newbranch" -b testos/buildmaster/x86_64-runtime -s "EOL redirect to new branch"

echo "ok new branch"

# Upgrade existing checkout
${CMD_PREFIX} ostree admin upgrade --os=testos --pull-only
${CMD_PREFIX} ostree admin upgrade --os=testos --deploy-only

# Check we got redirected to the new branch
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf "${bootcsum}"
rev=$(${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo rev-parse testos/buildmaster/newbranch)
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/usr/bin/content-iteration "1"

assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0.origin "newbranch"

echo "ok update and redirect"
