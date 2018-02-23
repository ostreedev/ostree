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

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..6"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy command"

# Commit + upgrade twice, so that we'll rotate out the original deployment
bootcsum1=${bootcsum}
os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
bootcsum2=${bootcsum}
os_repository_new_commit "1"
bootcsum3=${bootcsum}
${CMD_PREFIX} ostree admin upgrade --os=testos

newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_not_streq ${bootcsum1} ${bootcsum2}
assert_not_streq ${bootcsum2} ${bootcsum3}
assert_not_has_dir sysroot/boot/ostree/testos-${bootcsum1}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum2}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'

echo "ok deploy and GC /boot"

${CMD_PREFIX} ostree admin cleanup
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'

echo "ok manual cleanup"

assert_n_pinned() {
    local n=$1
    ${CMD_PREFIX} ostree admin status > status.txt
    local n_pinned="$(grep -F -c -e 'Pinned: yes' < status.txt)"
    if test "${n_pinned}" '!=' "${n}"; then
        cat status.txt
        fatal "${n_pinned} != ${n}"
    fi
}
assert_n_deployments() {
    local n=$1
    ${CMD_PREFIX} ostree admin status > status.txt
    local n_deployments="$(grep -F -c -e 'Version: ' < status.txt)"
    if test "${n_deployments}" '!=' "${n}"; then
        cat status.txt
        fatal "${n_deployments} != ${n}"
    fi
}
assert_n_pinned 0
${CMD_PREFIX} ostree admin pin 0
assert_n_pinned 1
${CMD_PREFIX} ostree admin pin -u 0
assert_n_pinned 0
echo "ok pin unpin"

${CMD_PREFIX} ostree admin pin 0
${CMD_PREFIX} ostree admin pin 1
assert_n_pinned 2
assert_n_deployments 2
os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_n_pinned 2
assert_n_deployments 3
echo "ok pin across upgrades"

${CMD_PREFIX} ostree admin pin -u 1
os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_n_pinned 1
assert_n_deployments 3
os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_n_pinned 1
assert_n_deployments 3

echo "ok pinning"
