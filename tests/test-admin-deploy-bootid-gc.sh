#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

echo "1..1"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
os_repository_new_commit

rm sysroot/ostree/repo/tmp/* -rf
export TEST_BOOTID=4072029c-8b10-60d1-d31b-8422eeff9b42
if env OSTREE_REPO_TEST_ERROR=pre-commit OSTREE_BOOTID=${TEST_BOOTID} \
       ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime 2>err.txt; then
    assert_not_reached "Should have hit OSTREE_REPO_TEST_ERROR_PRE_COMMIT"
fi
stagepath=$(ls -d sysroot/ostree/repo/tmp/staging-${TEST_BOOTID}-*)
assert_has_dir "${stagepath}"

# We have an older failed stage, now use a new boot id

export NEW_TEST_BOOTID=5072029c-8b10-60d1-d31b-8422eeff9b42
if env OSTREE_REPO_TEST_ERROR=pre-commit OSTREE_BOOTID=${NEW_TEST_BOOTID} \
       ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=FOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime 2>err.txt; then
    assert_not_reached "Should have hit OSTREE_REPO_TEST_ERROR_PRE_COMMIT"
fi
newstagepath=$(ls -d sysroot/ostree/repo/tmp/staging-${NEW_TEST_BOOTID}-*)
assert_has_dir "${newstagepath}"
env OSTREE_BOOTID=${NEW_TEST_BOOTID} ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
newstagepath=$(echo sysroot/ostree/repo/tmp/staging-${NEW_TEST_BOOTID}-*)
assert_not_has_dir "${stagepath}"
assert_not_has_dir "${newstagepath}"

echo "ok admin bootid GC"
