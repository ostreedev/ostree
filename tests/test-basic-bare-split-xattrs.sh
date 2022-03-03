#!/usr/bin/env bash
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

mode="bare-split-xattrs"
OSTREE="${CMD_PREFIX} ostree --repo=${test_tmpdir}/repo"

SUDO="sudo --non-interactive"
PRIVILEGED="false"
if [ $(id -u) -eq 0 ]; then
  PRIVILEGED="true"
  SUDO=""
elif $(${SUDO} -v); then
  PRIVILEGED="true"
fi

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

if [ "${PRIVILEGED}" = "true" ]; then
    COMMIT="d614c428015227259031b0f19b934dade908942fd71c49047e0daa70e7800a5d"
    cd ${test_tmpdir}
    ${SUDO} tar --same-permissions --same-owner -xaf ${test_srcdir}/fixtures/bare-split-xattrs/basic.tar.xz
    ${SUDO} ${OSTREE} fsck --all
    ${OSTREE} log ${COMMIT} > out.txt
    assert_file_has_content_literal out.txt "fixtures: bare-split-xattrs repo"
    ${OSTREE} ls ${COMMIT} -X /foo > out.txt
    assert_file_has_content_literal out.txt "{ @a(ayay) [] } /foo"
    ${OSTREE} ls ${COMMIT} -X /bar > out.txt
    assert_file_has_content_literal out.txt "{ [(b'user.mykey', [byte 0x6d, 0x79, 0x76, 0x61, 0x6c, 0x75, 0x65])] } /bar"
    ${OSTREE} ls ${COMMIT} /foolink > out.txt
    assert_file_has_content_literal out.txt "/foolink -> foo"
    tap_ok "reading simple fixture"
    ${SUDO} rm -rf -- repo log.txt
else
    tap_ok "reading simple fixture # skip Unable to sudo"
fi

tap_end
