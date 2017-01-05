#!/bin/bash
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

# See https://github.com/ostreedev/ostree/pull/642

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

setup_os_repository "archive-z2" "syslinux"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
parent_rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse ${rev}^)
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime@${parent_rev}
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/ostree/deploy/testos/deploy/${parent_rev}.0
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0
# Do a pull, this one should get us new content
${CMD_PREFIX} ostree admin upgrade --os=testos --pull-only --os=testos > out.txt
assert_not_file_has_content out.txt 'No update available'
# And pull again should still tell us we have new content
${CMD_PREFIX} ostree admin upgrade --os=testos --pull-only --os=testos > out.txt
assert_not_file_has_content out.txt 'No update available'
assert_has_dir sysroot/ostree/deploy/testos/deploy/${parent_rev}.0
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'TestOS 42 1.0.9'
assert_streq "${rev}" $(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
# Now, generate new content upstream; we shouldn't pull it
os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos --deploy-only --os=testos > out.txt
assert_not_file_has_content out.txt 'No update available'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'TestOS 42 1.0.10'
assert_has_dir sysroot/ostree/deploy/testos/deploy/${parent_rev}.0
assert_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0
${CMD_PREFIX} ostree admin upgrade --os=testos --deploy-only --os=testos > out.txt
assert_file_has_content out.txt 'No update available'

echo 'ok upgrade --pull-only + --deploy-only'
