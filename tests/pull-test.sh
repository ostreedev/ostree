#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

cd ${test_tmpdir}
mkdir repo
${CMD_PREFIX} ostree --repo=repo init
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull"

cd ${test_tmpdir}
$OSTREE checkout origin/main checkout-origin-main
cd checkout-origin-main
assert_file_has_content firstfile '^first$'
assert_file_has_content baz/cow '^moo$'
echo "ok pull contents"

cd ${test_tmpdir}
ostree --repo=ostree-srv/gnomerepo commit -b main -s "Metadata string" --add-detached-metadata-string=SIGNATURE=HANCOCK --tree=ref=main
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
$OSTREE show --print-detached-metadata-key=SIGNATURE main > main-meta
assert_file_has_content main-meta "HANCOCK"
echo "ok pull detached metadata"
