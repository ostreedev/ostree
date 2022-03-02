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

cd ${test_tmpdir}
mkdir -p "${test_tmpdir}/files"
touch files/foo
${OSTREE} init --mode "${mode}"
if ${OSTREE} commit --orphan -m "not implemented" files; then
    assert_not_reached "commit to bare-split-xattrs should have failed"
fi
${OSTREE} fsck --all
tap_ok "commit not implemented"
rm -rf -- repo files

cd ${test_tmpdir}
mkdir -p "${test_tmpdir}/files"
touch files/foo
${OSTREE} init --mode "${mode}"
OSTREE_EXP_WRITE_BARE_SPLIT_XATTRS=true ${OSTREE} commit --orphan -m "experimental" files
if ${OSTREE} fsck --all; then
    assert_not_reached "fsck should have failed"
fi
tap_ok "commit exp override"
rm -rf -- repo files

tap_end
