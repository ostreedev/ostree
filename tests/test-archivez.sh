#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..12'

setup_test_repository "archive"

. ${test_srcdir}/archive-test.sh

${CMD_PREFIX} ostree --repo=repo-archive-z2 init --mode=archive-z2
echo "ok did an init with archive-z2 alias"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2
${CMD_PREFIX} ostree --repo=repo2 remote add --set=gpg-verify=false aremote file://$(pwd)/repo test2
${CMD_PREFIX} ostree --repo=repo2 pull aremote
${CMD_PREFIX} ostree --repo=repo2 rev-parse aremote/test2
${CMD_PREFIX} ostree --repo=repo2 fsck
echo "ok pull with from file:/// uri"
