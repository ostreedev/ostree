#!/bin/bash
# Test min-free-space-percent using loopback devices

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/../libinsttest.sh
date

prepare_tmpdir
trap _tmpdir_cleanup EXIT

cd ${test_tmpdir}
truncate -s 20MB testblk.img
blkdev=$(losetup --find --show $(pwd)/testblk.img)
mkfs.xfs ${blkdev}
mkdir mnt
mount ${blkdev} mnt

# first test min-free-space-percent
ostree --repo=mnt/repo init --mode=bare-user
echo 'fsync=false' >> mnt/repo/config
if ostree --repo=mnt/repo pull-local /ostree/repo ${host_commit} 2>err.txt; then
    fatal "succeeded in doing a pull with no free space"
fi
assert_file_has_content err.txt "min-free-space-percent"
echo "ok min-free-space-percent"

# now test min-free-space-size
rm -rf mnt/repo
ostree --repo=mnt/repo init --mode=bare-user
echo 'fsync=false' >> mnt/repo/config
echo 'min-free-space-size=10MB' >> mnt/repo/config
if ostree --repo=mnt/repo pull-local /ostree/repo ${host_commit} 2>err.txt; then
    fatal "succeeded in doing a pull with no free space"
fi
assert_file_has_content err.txt "min-free-space-size"
echo "ok min-free-space-size (error)"

umount mnt
losetup -d ${blkdev}
rm ${blkdev}

# min-free-space-size success
ostree --repo=repo init --mode=bare-user
echo 'fsync=false' >> repo/config
echo 'min-free-space-size=1MB' >> repo/config
ostree --repo=repo pull-local /ostree/repo ${host_commit}
echo "ok min-free-space-size (success)"

date
