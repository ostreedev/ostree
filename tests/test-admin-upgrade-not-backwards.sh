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

echo "1..2"

ref=testos/buildmaster/x86_64-runtime
cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos ${ref}
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse ${ref})
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:${ref}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

# This should be a no-op
${CMD_PREFIX} ostree admin upgrade --os=testos

# Generate a new commit with an older timestamp that also has
# some new content, so we test timestamp checking during pull
# <https://github.com/ostreedev/ostree/pull/1055>
origrev=$(ostree --repo=${test_tmpdir}/sysroot/ostree/repo rev-parse testos:${ref})
cd ${test_tmpdir}/osdata
echo "new content for pull timestamp checking" > usr/share/test-pull-ts-check.txt
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=tscheck" \
              -b ${ref} --timestamp='October 25 1985'
newrev=$(ostree --repo=${test_tmpdir}/testos-repo rev-parse ${ref})
assert_not_streq ${origrev} ${newrev}
cd ${test_tmpdir}
tscheck_checksum=$(ostree_file_path_to_checksum testos-repo ${ref} /usr/share/test-pull-ts-check.txt)
tscheck_filez_objpath=$(ostree_checksum_to_relative_object_path testos-repo ${tscheck_checksum})
assert_has_file testos-repo/${tscheck_filez_objpath}
if ${CMD_PREFIX} ostree admin upgrade --os=testos 2>upgrade-err.txt; then
    assert_not_reached 'upgrade unexpectedly succeeded'
fi
assert_file_has_content upgrade-err.txt 'chronologically older'
currev=$(ostree --repo=sysroot/ostree/repo rev-parse testos:${ref})
assert_not_streq ${newrev} ${currev}
assert_streq ${origrev} ${currev}
tscheck_file_objpath=$(ostree_checksum_to_relative_object_path sysroot/ostree/repo ${tscheck_checksum})
assert_not_has_file sysroot/ostree/repo/${tscheck_file_objpath}

echo 'ok upgrade will not go backwards'

${CMD_PREFIX} ostree admin upgrade --os=testos --allow-downgrade

echo 'ok upgrade backwards'
