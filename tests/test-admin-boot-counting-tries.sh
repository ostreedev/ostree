#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_os_repository "archive" "syslinux"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.boot-counting-tries 3
v=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config get sysroot.boot-counting-tries)
assert_streq "$v" 3

tap_ok "init boot counting tries"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
export rev

${CMD_PREFIX} ostree admin deploy --karg=quiet --stateroot=testos testos:testos/buildmain/x86_64-runtime
entry=$(ls sysroot/boot/loader/entries/)
assert_streq "${entry}" ostree-1+3.conf

tap_ok "deploy with boot counting"

tap_end
