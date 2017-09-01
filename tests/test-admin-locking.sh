#!/bin/bash
#
# Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

# If parallel is not installed, skip the test
if ! parallel --gnu /bin/true </dev/null >/dev/null 2>&1; then
    echo "1..0 # SKIP GNU parallel not available"
    exit 0
fi    

echo "1..1"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

parallel_cmd="parallel --gnu"
if parallel --help | grep -q -e --no-notice; then
    parallel_cmd="${parallel_cmd} --no-notice"
fi

count=$(($(getconf _NPROCESSORS_ONLN) * 2))
seq "${count}" | ${parallel_cmd} -n0 ${CMD_PREFIX} ostree admin deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime

${CMD_PREFIX} ostree admin status > status.txt
grep "testos ${rev}" status.txt | wc -l > status-matches.txt
assert_file_has_content status-matches.txt $((${count} + 1))

echo 'ok deploy locking'
