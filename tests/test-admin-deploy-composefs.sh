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

skip_without_ostree_feature composefs

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

cd osdata
mkdir -p usr/lib/ostree
cat > usr/lib/ostree/prepare-root.conf << 'EOF'
[composefs]
enabled=true
EOF
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.composefs -b testos/buildmain/x86_64-runtime
cd -
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime

${CMD_PREFIX} ostree admin deploy --os=testos --karg=root=LABEL=foo --karg=testkarg=1 testos:testos/buildmain/x86_64-runtime
ls sysroot/ostree/deploy/testos/deploy/*.0/.ostree.cfs

tap_ok composefs

tap_end
