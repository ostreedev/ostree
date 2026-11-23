#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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
setup_os_repository "archive" "grub2 ostree-grub-generator"

extra_admin_tests=1

. $(dirname $0)/admin-test.sh

# A single quote in the title (here from the commit version, but PRETTY_NAME
# feeds it too) used to end the GRUB quoted string, and a % was eaten by
# printf, since the generator spliced the entry fields into its format string.
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "grub2 ostree-grub-generator"
os_repository_new_commit 0 0 testos/buildmain/x86_64-runtime "1.0'quote 100%s"
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --os=testos testos:testos/buildmain/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-1.conf "^title TestOS 42 1.0'quote 100%s (ostree:0)$"
assert_file_has_content_literal sysroot/boot/grub2/grub.cfg "menuentry 'TestOS 42 1.0'\\''quote 100%s (ostree:0)' {"
assert_not_file_has_content sysroot/boot/grub2/grub.cfg "menuentry 'TestOS 42 1.0'quote"
echo "ok grub2 title quoting"
