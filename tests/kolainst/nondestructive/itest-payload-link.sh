#!/bin/bash
# FIXME just for webserver
# kola: { "tags": "needs-internet" }
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

echo "1..1"
date

# Use /var/tmp so we have O_TMPFILE etc.
prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT
# We use this user down below, it needs access too
setfacl -d -m u:bin:rwX .
setfacl -m u:bin:rwX .
ostree --repo=repo init --mode=archive
echo -e '[archive]\nzlib-level=1\n' >> repo/config

mkdir content
cd content
dd if=/dev/urandom of=bigobject bs=4k count=2560
cp --reflink=auto bigobject bigobject2
# Different metadata, same content
chown bin:bin bigobject2
cd ..
ostree --repo=repo commit -b dupobjects --consume --selinux-policy=/ --tree=dir=content
ostree --repo=repo summary -u

run_tmp_webserver $(pwd)/repo

origin=$(cat ${test_tmpdir}/httpd-address)

cleanup() {
    cd ${test_tmpdir}
    for mnt in ${mnts:-}; do
        umount ${mnt} || true
    done
    for blkdev in ${blkdevs:-}; do
        losetup -d ${blkdev} || true
    done
}
trap cleanup EXIT

truncate -s 2G testblk1.img
blkdev1=$(losetup --find --show $(pwd)/testblk1.img)
blkdevs="${blkdev1}"
# This filesystem must support reflinks
mkfs.xfs -m reflink=1 ${blkdev1}
mkdir mnt1
mount ${blkdev1} mnt1
mnts=mnt1

truncate -s 2G testblk2.img
blkdev2=$(losetup --find --show $(pwd)/testblk2.img)
blkdevs="${blkdev1} ${blkdev2}"
mkfs.xfs -m reflink=1 ${blkdev2}
mkdir mnt2
mount ${blkdev2} mnt2
mnts="mnt1 mnt2"

cd mnt1
# See above for setfacl rationale
setfacl -d -m u:bin:rwX .
setfacl -m u:bin:rwX .
ls -al .
runuser -u bin mkdir foo
runuser -u bin touch foo/bar
ls -al foo

# Test that reflink is really there (not just --reflink=auto)
touch a
cp --reflink a b
mkdir repo
ostree --repo=repo init
ostree config --repo=repo set core.payload-link-threshold 0
ostree --repo=repo remote add origin --set=gpg-verify=false ${origin}
ostree --repo=repo pull --disable-static-deltas origin dupobjects
find repo -type l -name '*.payload-link' >payload-links.txt
assert_streq "$(wc -l < payload-links.txt)" "1"

cat payload-links.txt | while read i; do
    payload_checksum=$(basename $(dirname $i))$(basename $i .payload-link)
    payload_checksum_calculated=$(sha256sum $(readlink -f $i) | cut -d ' ' -f 1)
    assert_streq "${payload_checksum}" "${payload_checksum_calculated}"
done
echo "ok payload link"

ostree --repo=repo checkout dupobjects content
# And another object which differs just in metadata
cp --reflink=auto content/bigobject{,3}
chown operator:0 content/bigobject3
cat >unpriv-child-repo.sh <<EOF
#!/bin/bash
# Check that commit to an user repo that has a parent still works
set -xeuo pipefail
ostree --repo=child-repo init --mode=bare-user
ostree --repo=child-repo remote add origin --set=gpg-verify=false ${origin}
ostree --repo=child-repo config set core.parent $(pwd)/repo
ostree --repo=child-repo commit -b test content
EOF
chmod a+x unpriv-child-repo.sh
runuser -u bin ./unpriv-child-repo.sh
find child-repo -type l -name '*.payload-link' >payload-links.txt
assert_streq "$(wc -l < payload-links.txt)" "0"
rm content -rf

echo "ok reflink unprivileged with parent repo"

# We can't reflink across devices though
cd ../mnt2
ostree --repo=repo init --mode=archive
ostree --repo=repo config set core.parent $(cd ../mnt1/repo && pwd)
ostree --repo=../mnt1/repo checkout dupobjects content
ostree --repo=repo commit -b dupobjects2 --consume --tree=dir=content
ostree --repo=repo pull --disable-static-deltas origin dupobjects
find repo -type l -name '*.payload-link' >payload-links.txt
assert_streq "$(wc -l < payload-links.txt)" "0"

echo "ok payload link across devices"

date
