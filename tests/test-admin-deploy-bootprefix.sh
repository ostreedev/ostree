#!/bin/bash
#
# Copyright (C) 2022 Colin Walters <walters@verbum.org>
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

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootprefix 'true'
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=root --os=testos testos:testos/buildmain/x86_64-runtime
assert_file_has_content_literal sysroot/boot/loader/entries/ostree-1-testos.conf 'linux /boot/ostree/testos-'
assert_file_has_content_literal sysroot/boot/loader/entries/ostree-1-testos.conf 'initrd /boot/ostree/testos-'

tap_ok "bootprefix"

tap_end
