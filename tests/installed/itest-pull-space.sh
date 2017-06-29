#!/bin/bash
# Test min-free-space-percent using loopback devices

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

test_tmpdir=$(prepare_tmpdir)
trap _tmpdir_cleanup EXIT

cd ${test_tmpdir}
truncate -s 100MB testblk.img
blkdev=$(losetup --find --show $(pwd)/testblk.img)
mkfs.xfs ${blkdev}
mkdir mnt
mount ${blkdev} mnt
ostree --repo=mnt/repo init --mode=bare-user
host_nonremoteref=$(echo ${host_refspec} | sed 's,[^:]*:,,')
if ostree --repo=mnt/repo pull-local /ostree/repo ${host_nonremoteref} 2>err.txt; then
    fatal "succeeded in doing a pull with no free space"
fi
assert_file_has_content err.txt "min-free-space-percent"
umount mnt
losetup -d ${blkdev}
