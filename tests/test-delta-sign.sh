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

bindatafiles="bash true ostree"

echo '1..7'

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1

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

${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev}
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} dummysign > show-not-signed.txt 2>&1 && exit 1
assert_file_has_content show-not-signed.txt "Verification fails"
assert_file_has_content show-not-signed.txt "no signatures in static-delta"

deltaprefix=$(get_assert_one_direntry_matching repo/deltas '.')
deltadir=$(get_assert_one_direntry_matching repo/deltas/${deltaprefix} '-')

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} dummysign > show-inline-not-signed.txt 2>&1 && exit 1
assert_file_has_content show-not-signed.txt "Verification fails"
assert_file_has_content show-not-signed.txt "no signatures in static-delta"

echo 'ok verify ok with unsigned deltas'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=dummy --sign=dummysign
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} dummysign > show-dummy-signed.txt
assert_file_has_content show-dummy-signed.txt "Verification OK"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=dummy --sign=dummysign
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} dummysign > show-dummy-inline-signed.txt
assert_file_has_content show-dummy-inline-signed.txt "Verification OK"

echo 'ok verified with dummy'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --sign-type=dummy --sign=dummysign
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} badsign > show-dummy-bad-signed.txt && exit 1
assert_file_has_content show-dummy-bad-signed.txt "Verification fails"

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline --sign-type=dummy --sign=dummysign
${CMD_PREFIX} ostree --repo=repo static-delta verify --sign-type=dummy ${origrev}-${newrev} badsign > show-dummy-bad-inline-signed.txt && exit 1
assert_file_has_content show-dummy-bad-inline-signed.txt "Verification fails"

echo 'ok verification failed with dummy and bad key'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline repo/deltas/${deltaprefix}/${deltadir}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline with no signature verification and no key'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 config set core.sign-verify-deltas true
${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline repo/deltas/${deltaprefix}/${deltadir} 2> apply-offline-verification-no-key.txt && exit 1
assert_file_has_content apply-offline-verification-no-key.txt "Key is mandatory to check delta signature"

echo 'ok apply offline failed with signature verification forced and no key'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline --sign-type=dummy repo/deltas/${deltaprefix}/${deltadir} dummysign
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline with dummy'

rm -rf repo2
ostree_repo_init repo2 --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline --sign-type=dummy repo/deltas/${deltaprefix}/${deltadir} badsign 2> apply-offline-bad-key.txt && exit 1
assert_file_has_content apply-offline-bad-key.txt "signature: dummy: incorrect signature"

echo 'ok apply offline failed with dummy and bad key'
