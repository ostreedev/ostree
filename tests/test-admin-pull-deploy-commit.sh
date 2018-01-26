#!/bin/bash
#
# Copyright (C) 2015 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

# See https://github.com/GNOME/ostree/pull/145

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

setup_os_repository "archive" "syslinux"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
parent_rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse ${rev}^)
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull testos ${parent_rev}
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos ${parent_rev}

echo 'ok deploy pulled commit'
