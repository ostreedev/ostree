#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

bindatafiles="bash true ostree"
morebindatafiles="false ls"

echo '1..3'

mkdir repo
${CMD_PREFIX} ostree --repo=repo init --mode=archive-z2

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

${CMD_PREFIX} ostree --repo=repo static-delta generate --empty --to=${origrev}
${CMD_PREFIX} ostree --repo=repo static-delta list | grep ${origrev} || exit 1
${CMD_PREFIX} ostree --repo=repo prune
${CMD_PREFIX} ostree --repo=repo static-delta list | grep ${origrev} || exit 1

permuteDirectory 1 files
${CMD_PREFIX} ostree --repo=repo commit -b test -s test --tree=dir=files

newrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)

${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline

deltaprefix=$(get_assert_one_direntry_matching repo/deltas '.')
deltadir=$(get_assert_one_direntry_matching repo/deltas/${deltaprefix} '-')

assert_has_file repo/deltas/${deltaprefix}/${deltadir}/superblock
assert_not_has_file repo/deltas/${deltaprefix}/${deltadir}/0

${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev}

assert_has_file repo/deltas/${deltaprefix}/${deltadir}/superblock
assert_has_file repo/deltas/${deltaprefix}/${deltadir}/0

${CMD_PREFIX} ostree --repo=repo static-delta generate --disable-bsdiff --from=${origrev} --to=${newrev} 2>&1 | grep "bsdiff=0 objects"
${CMD_PREFIX} ostree --repo=repo static-delta generate --max-bsdiff-size=0 --from=${origrev} --to=${newrev} 2>&1 | grep "bsdiff=0 objects"
${CMD_PREFIX} ostree --repo=repo static-delta generate --max-bsdiff-size=10000 --from=${origrev} --to=${newrev} 2>&1 | grep "bsdiff=[1-9]"

${CMD_PREFIX} ostree --repo=repo static-delta list | grep ${origrev}-${newrev} || exit 1

if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --empty 2>>err.txt; then
    assert_not_reached "static-delta generate --from=${origrev} --empty unexpectedly succeeded"
fi

echo 'ok generate'

${CMD_PREFIX} ostree --repo=repo static-delta show ${origrev}-${newrev} > show.txt
assert_file_has_content show.txt 'Endianness: \(little\|big\)'

echo 'ok show'

mkdir repo2 && ${CMD_PREFIX} ostree --repo=repo2 init --mode=archive-z2
${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${newrev}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok pull delta'

rm repo2 -rf
mkdir repo2 && ${CMD_PREFIX} ostree --repo=repo2 init --mode=bare-user
mkdir deltadir

deltaprefix=$(get_assert_one_direntry_matching repo/deltas '.')
deltadir=$(get_assert_one_direntry_matching repo/deltas/${deltaprefix} '-')
${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline repo/deltas/${deltaprefix}/${deltadir}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline'

rm -rf repo/deltas/${deltaprefix}/${deltadir}/*
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --inline
assert_not_has_file repo/deltas/${deltaprefix}/${deltadir}/0

rm repo2 -rf
mkdir repo2 && ostree --repo=repo2 init --mode=bare-user

${CMD_PREFIX} ostree --repo=repo2 pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo2 ls ${origrev} >/dev/null
${CMD_PREFIX} ostree --repo=repo2 static-delta apply-offline repo/deltas/${deltaprefix}/${deltadir}
${CMD_PREFIX} ostree --repo=repo2 fsck
${CMD_PREFIX} ostree --repo=repo2 ls ${newrev} >/dev/null

echo 'ok apply offline inline'
