#!/bin/bash
#
# Copyright (C) 2024 Red Hat, Inc.
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

set -euox pipefail

. $(dirname $0)/libtest.sh

if ! has_ostree_feature initial-var; then
    fatal missing initial-var
fi

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "initial ls"
ls -R sysroot/ostree/deploy/testos/var

cd osdata
mkdir -p var/lib/
echo somedata > var/lib/somefile
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmain/x86_64-runtime
cd -
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
ls -R sysroot/ostree/deploy/testos/var
assert_file_has_content sysroot/ostree/deploy/testos/var/lib/somefile somedata
# We don't have tmpfiles here yet
assert_not_has_dir sysroot/ostree/deploy/*.0/usr/lib/tmpfiles.d
if ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo ls testos/buildmain/x86_64-runtime /var/log; then
    fatal "var/log in commit"
fi
# This one is created via legacy init w/o tmpfiles.d
assert_has_dir sysroot/ostree/deploy/testos/var/log

tap_ok deployment var init

cd osdata
echo someotherdata > var/lib/someotherfile
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmain/x86_64-runtime
cd -
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
assert_not_has_file sysroot/ostree/deploy/testos/var/lib/someotherfile

tap_ok deployment var not updated

${CMD_PREFIX} ostree admin undeploy 0
${CMD_PREFIX} ostree admin undeploy 0
rm sysroot/ostree/deploy/testos/var/* -rf

cd osdata
mkdir -p usr/lib/tmpfiles.d
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmain/x86_64-runtime
cd -
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime

# Not in the commit, so not created via legacy init because we have tmpfiles.d
assert_not_has_dir sysroot/ostree/deploy/testos/var/log

tap_ok deployment var w/o legacy

tap_end
