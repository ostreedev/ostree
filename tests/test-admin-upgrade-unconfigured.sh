#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..2"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
echo "unconfigured-state=Use \"subscription-manager\" to enable online updates for example.com OS" >> sysroot/ostree/deploy/testos/deploy/${rev}.0.origin

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
if ${CMD_PREFIX} ostree admin upgrade --os=testos 2>err.txt; then
    assert_not_reached "upgrade unexpectedly succeeded"
fi
assert_file_has_content err.txt "Use.*subscription.*online"

echo "ok error"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false otheros file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin switch --os=testos otheros:testos/buildmaster/x86_64-runtime

echo "ok switch"
