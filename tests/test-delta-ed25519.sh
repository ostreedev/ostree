#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+
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

skip_without_user_xattrs

skip_without_sign_ed25519

bindatafiles="bash true ostree"

echo '1..12'

mkdir repo
ostree_repo_init repo --mode=archive

mkdir files
for bin in ${bindatafiles}; do
    cp $(which ${bin}) files
done

${CMD_PREFIX} ostree --repo=repo commit -b test -s test --tree=dir=files

function permuteFile() {
    permutation=$(($1 % 2))
    output=$2
    case $permutation in
	0) dd if=/dev/zero count=40 bs=1 >> $output;;
	1) echo aheader | cat - $output >> $output.new && mv $output.new $output;;
    esac
}

function permuteDirectory() {
    permutation=$1
    dir=$2
    for x in ${dir}/*; do
	for z in $(seq ${permutation}); do
	    permuteFile ${z} ${x}
	done
    done
}

get_assert_one_direntry_matching() {
    local path=$1
    local r=$2
    local child=""
    local bn
    for p in ${path}/*; do
	bn=$(basename $p)
	if ! echo ${bn} | grep -q "$r"; then
	    continue
	fi
	if test -z "${child}"; then
	    child=${bn}
	else
	    assert_not_reached "Expected only one child matching ${r} in ${path}";
	fi
    done
    if test -z "${child}"; then
	assert_not_reached "Failed to find child matching ${r}"
    fi
    echo ${child}
}

origrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)

permuteDirectory 1 files
${CMD_PREFIX} ostree --repo=repo commit -b test -s test --tree=dir=files

newrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)

# Test ostree sign with 'ed25519' module
gen_ed25519_keys
PUBLIC=${ED25519PUBLIC}
SEED=${ED25519SEED}
SECRET=${ED25519SECRET}
WRONG_PUBLIC="$(gen_ed25519_random_public)"

SECRETKEYS="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.ed25519)"
echo ${SECRET} > ${SECRETKEYS}

${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-key-signed-1.txt
assert_file_has_content show-ed25519-key-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" "${WRONG_PUBLIC}" > show-ed25519-key-signed-2.txt
assert_file_has_content show-ed25519-key-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" "${PUBLIC}" > show-ed25519-key-signed-3.txt
assert_file_has_content show-ed25519-key-signed-3.txt "Verification OK"

deltaprefix=$(get_assert_one_direntry_matching repo/deltas '.')
deltadir=$(get_assert_one_direntry_matching repo/deltas/${deltaprefix} '-')

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-key-inline-signed-1.txt
assert_file_has_content show-ed25519-key-inline-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" "${WRONG_PUBLIC}" > show-ed25519-key-inline-signed-2.txt
assert_file_has_content show-ed25519-key-inline-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" "${PUBLIC}" > show-ed25519-key-inline-signed-3.txt
assert_file_has_content show-ed25519-key-inline-signed-3.txt "Verification OK"

echo 'ok verified with ed25519 (sign - key)'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-keyfile-signed-1.txt
assert_file_has_content show-ed25519-keyfile-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" "${WRONG_PUBLIC}" > show-ed25519-keyfile-signed-2.txt
assert_file_has_content show-ed25519-keyfile-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" "${PUBLIC}" > show-ed25519-keyfile-signed-3.txt
assert_file_has_content show-ed25519-keyfile-signed-3.txt "Verification OK"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-keyfile-inline-signed-1.txt
assert_file_has_content show-ed25519-keyfile-inline-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" "${WRONG_PUBLIC}" > show-ed25519-keyfile-inline-signed-2.txt
assert_file_has_content show-ed25519-keyfile-inline-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" "${PUBLIC}" > show-ed25519-keyfile-inline-signed-3.txt
assert_file_has_content show-ed25519-keyfile-inline-signed-3.txt "Verification OK"

echo 'ok verified with ed25519 (keyfile - key)'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-key-bad-signed.txt && exit 1
assert_file_has_content show-ed25519-key-bad-signed.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-key-bad-inline-signed.txt && exit 1
assert_file_has_content show-ed25519-key-bad-inline-signed.txt "Verification fails"

echo 'ok Verification fails with ed25519 (sign - bad key)'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-keyfile-bad-signed.txt && exit 1
assert_file_has_content show-ed25519-keyfile-bad-signed.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-keyfile-bad-inline-signed.txt && exit 1
assert_file_has_content show-ed25519-keyfile-bad-inline-signed.txt "Verification fails"

echo 'ok Verification fails with ed25519 (keyfile - bad key)'

# Prepare files with public ed25519 signatures
PUBKEYS="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.ed25519)"
for((i=0;i<100;i++)); do
    # Generate a list with some public signatures
    gen_ed25519_random_public
done > ${PUBKEYS}

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-bad-signed-1.txt && exit 1
assert_file_has_content show-ed25519-file-bad-signed-1.txt "Verification fails"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-bad-signed-2.txt && exit 1
assert_file_has_content show-ed25519-file-bad-signed-2.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-inline-bad-signed-1.txt && exit 1
assert_file_has_content show-ed25519-file-inline-bad-signed-1.txt "Verification fails"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-inline-bad-signed-2.txt && exit 1
assert_file_has_content show-ed25519-file-inline-bad-signed-2.txt "Verification fails"

echo 'ok Verification fails with ed25519 (sign - bad keys)'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-bad-signed-3.txt && exit 1
assert_file_has_content show-ed25519-file-bad-signed-3.txt "Verification fails"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-bad-signed-4.txt && exit 1
assert_file_has_content show-ed25519-file-bad-signed-4.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-inline-bad-signed-3.txt && exit 1
assert_file_has_content show-ed25519-file-inline-bad-signed-3.txt "Verification fails"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-inline-bad-signed-4.txt && exit 1
assert_file_has_content show-ed25519-file-inline-bad-signed-4.txt "Verification fails"

echo 'ok Verification fails with ed25519 (keyfile - bad keys)'

# Add correct key into the list
echo ${PUBLIC} >> ${PUBKEYS}

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-signed-1.txt
assert_file_has_content show-ed25519-file-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-signed-2.txt
assert_file_has_content show-ed25519-file-signed-2.txt "Verification OK"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --sign=${SECRET}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-inline-signed-1.txt
assert_file_has_content show-ed25519-file-inline-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-inline-signed-2.txt
assert_file_has_content show-ed25519-file-inline-signed-2.txt "Verification OK"

echo 'ok verified with ed25519 (sign - file)'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-signed-3.txt
assert_file_has_content show-ed25519-file-signed-3.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-signed-4.txt
assert_file_has_content show-ed25519-file-signed-4.txt "Verification OK"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-file-inline-signed-3.txt
assert_file_has_content show-ed25519-file-inline-signed-3.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-file-inline-signed-4.txt
assert_file_has_content show-ed25519-file-inline-signed-4.txt "Verification OK"

echo 'ok verified with ed25519 (keyfile - file)'

# Test ostree sign with multiple 'ed25519' keys
gen_ed25519_keys
PUBLIC2=${ED25519PUBLIC}
SEED2=${ED25519SEED}
SECRET2=${ED25519SECRET}

echo ${SECRET2} >> ${SECRETKEYS}
echo ${PUBLIC2} >> ${PUBKEYS}

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-multiplekeys-signed-1.txt
assert_file_has_content show-ed25519-multiplekeys-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC2}" > show-ed25519-multiplekeys-signed-2.txt
assert_file_has_content show-ed25519-multiplekeys-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-multiplekeys-bad-signed.txt && exit 1
assert_file_has_content show-ed25519-multiplekeys-bad-signed.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-multiplekeys-signed-3.txt
assert_file_has_content show-ed25519-multiplekeys-signed-3.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-multiplekeys-signed-4.txt
assert_file_has_content show-ed25519-multiplekeys-signed-4.txt "Verification OK"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC}" > show-ed25519-multiplekeys-inline-signed-1.txt
assert_file_has_content show-ed25519-multiplekeys-inline-signed-1.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${PUBLIC2}" > show-ed25519-multiplekeys-inline-signed-2.txt
assert_file_has_content show-ed25519-multiplekeys-inline-signed-2.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} "${WRONG_PUBLIC}" > show-ed25519-multiplekeys-bad-inline-signed.txt && exit 1
assert_file_has_content show-ed25519-multiplekeys-bad-inline-signed.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=ed25519 --keys-file=${SECRETKEYS}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} > show-ed25519-multiplekeys-inline-signed-3.txt
assert_file_has_content show-ed25519-multiplekeys-inline-signed-3.txt "Verification OK"
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=ed25519 ${origrev}-${newrev} --keys-file=${PUBKEYS} "${WRONG_PUBLIC}" > show-ed25519-multiplekeys-inline-signed-4.txt
assert_file_has_content show-ed25519-multiplekeys-inline-signed-4.txt "Verification OK"

echo 'ok verified with ed25519 (multiple keys)'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline --sign-type=ed25519 --keys-file=${PUBKEYS} repo/deltas/${deltaprefix}/${deltadir}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline with ed25519 (keyfile)'

mkdir -p ${test_tmpdir}/{trusted,revoked}.ed25519.d

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

echo ${PUBLIC} > ${test_tmpdir}/trusted.ed25519.d/correct
${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline --keys-dir=${test_tmpdir} repo/deltas/${deltaprefix}/${deltadir}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline with ed25519 (keydir)'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

echo ${PUBLIC} > ${test_tmpdir}/revoked.ed25519.d/correct
${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
if ${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline --keys-dir=${test_tmpdir} repo/deltas/${deltaprefix}/${deltadir}; then
  exit 1
fi

rm -rf ${test_tmpdir}/{trusted,revoked}.ed25519.d

echo 'ok apply offline with ed25519 revoking key mechanism (keydir)'
