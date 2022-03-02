#!/usr/bin/env bash
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

mode="bare-split-xattrs"
OSTREE="${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo"

cd ${test_tmpdir}
${OSTREE} init --mode "${mode}"
${OSTREE} config get core.mode > mode.txt
assert_file_has_content mode.txt "${mode}"
tap_ok "repo init"
rm -rf -- repo mode.txt

cd ${test_tmpdir}
${OSTREE} init --mode "${mode}"
${OSTREE} fsck --all
tap_ok "repo fsck"
rm -rf -- repo

tap_end
