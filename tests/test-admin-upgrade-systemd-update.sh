#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
setup_os_repository "archive-z2" "syslinux"

echo "1..2"

# Setup a deployment
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0/usr
assert_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0/etc
assert_has_dir sysroot/ostree/deploy/testos/var
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/.updated
assert_not_has_file sysroot/ostree/deploy/testos/var/.updated

echo "ok deploy"

# Create the /etc/.updated and /var/.updated files with /usr modification time
usr=sysroot/ostree/deploy/testos/deploy/${rev}.0/usr
touch -r ${usr} sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/.updated
touch -r ${usr} sysroot/ostree/deploy/testos/var/.updated

# Make a new commit, upgrade and ensure .updated files are gone in the
# new deployment but /etc/.updated still exists in the previous
# (current) deployment
os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/.updated
assert_not_has_file sysroot/ostree/deploy/testos/var/.updated
assert_has_file sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/.updated

echo "ok .updated files removed"
