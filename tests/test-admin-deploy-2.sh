#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..3"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy command"

# Commit + upgrade twice, so that we'll rotate out the original deployment
bootcsum1=${bootcsum}
os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
bootcsum2=${bootcsum}
os_repository_new_commit "1"
bootcsum3=${bootcsum}
${CMD_PREFIX} ostree admin upgrade --os=testos

newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_not_streq ${bootcsum1} ${bootcsum2}
assert_not_streq ${bootcsum2} ${bootcsum3}
assert_not_has_dir sysroot/boot/ostree/testos-${bootcsum1}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum2}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'

echo "ok deploy and GC /boot"

${CMD_PREFIX} ostree admin cleanup
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'

echo "ok manual cleanup"
