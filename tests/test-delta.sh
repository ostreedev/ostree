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

set -e

. $(dirname $0)/libtest.sh

bindatafiles="bash true ostree"
morebindatafiles="false ls"

echo '1..2'

mkdir repo
ostree --repo=repo init --mode=archive-z2

mkdir files
for bin in ${bindatafiles}; do
    cp $(which ${bin}) files
done

ostree --repo=repo commit -b test -s test --tree=dir=files

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

origrev=$(ostree --repo=repo rev-parse test)

ostree --repo=repo static-delta generate --empty --to=${origrev}
ostree --repo=repo static-delta list | grep ${origrev} || exit 1

permuteDirectory 1 files
ostree --repo=repo commit -b test -s test --tree=dir=files

newrev=$(ostree --repo=repo rev-parse test)
ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev}

ostree --repo=repo static-delta list | grep ${origrev}-${newrev} || exit 1

if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --empty 2>>err.txt; then
    assert_not_reached "static-delta generate --from=${origrev} --empty unexpectedly succeeded"
fi

mkdir repo2
ostree --repo=repo2 init --mode=archive-z2
ostree --repo=repo2 pull-local repo ${origrev}
