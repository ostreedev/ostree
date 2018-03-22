#!/bin/bash
#
# Copyright (C) 2018 Red Hat, Inc.
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

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

echo "1..1"

# Use /var/tmp so we have O_TMPFILE etc.
prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT
ostree --repo=repo init --mode=archive
echo -e '[archive]\nzlib-level=1\n' >> repo/config
host_nonremoteref=$(echo ${host_refspec} | sed 's,[^:]*:,,')
ostree --repo=repo pull-local /ostree/repo ${host_commit}
ostree --repo=repo refs ${host_commit} --create=${host_nonremoteref}

run_tmp_webserver $(pwd)/repo

origin=$(cat ${test_tmpdir}/httpd-address)

cleanup() {
    cd ${test_tmpdir}
    umount mnt || true
    test -n "${blkdev:-}" && losetup -d ${blkdev} || true
}
trap cleanup EXIT

mkdir mnt
truncate -s 2G testblk.img
if ! blkdev=$(losetup --find --show $(pwd)/testblk.img); then
    echo "ok # SKIP not run when cannot setup loop device"
    exit 0
fi

mkfs.xfs -m reflink=1 ${blkdev}

mount ${blkdev} mnt
cd mnt

# Test that coreutils will do reflink
touch a
cp --reflink a b
mkdir repo
ostree --repo=repo init
ostree config --repo=repo set core.payload-link-threshold 0
ostree --repo=repo remote add origin --set=gpg-verify=false ${origin}
ostree --repo=repo pull --disable-static-deltas origin ${host_nonremoteref}
find repo -type l -name '*.payload-link' >payload-links.txt
assert_not_streq "$(wc -l < payload-links.txt)" "0"

# Disable logging for inner loop, otherwise it'd be enormous
set +x
cat payload-links.txt | while read i; do
    payload_checksum=$(basename $(dirname $i))$(basename $i .payload-link)
    payload_checksum_calculated=$(sha256sum $(readlink -f $i) | cut -d ' ' -f 1)
    assert_streq "${payload_checksum}" "${payload_checksum_calculated}"
done
set -x
echo "ok pull creates .payload-link"
