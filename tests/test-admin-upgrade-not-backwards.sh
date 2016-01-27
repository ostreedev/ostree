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

echo "1..1"

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

echo "1..2"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

# This should be a no-op
${CMD_PREFIX} ostree admin upgrade --os=testos

# Now reset to an older revision
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo reset testos/buildmaster/x86_64-runtime{,^}

if ${CMD_PREFIX} ostree admin upgrade --os=testos 2>upgrade-err.txt; then
    assert_not_reached 'upgrade unexpectedly succeeded'
fi
assert_file_has_content upgrade-err.txt 'chronologically older'

echo 'ok upgrade will not go backwards'

${CMD_PREFIX} ostree admin upgrade --os=testos --allow-downgrade

echo 'ok upgrade backwards'
