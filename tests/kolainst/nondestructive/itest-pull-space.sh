#!/bin/bash
# Test min-free-space-percent using loopback devices

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh
date

prepare_tmpdir
truncate -s 20MiB testblk.img
blkdev=$(losetup --find --show $(pwd)/testblk.img)
mkfs.ext4 -m 0 ${blkdev}
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


# min-free-space-size success
ostree --repo=repo init --mode=bare-user
echo 'fsync=false' >> repo/config
echo 'min-free-space-size=1MB' >> repo/config
ostree --repo=repo pull-local /ostree/repo ${host_commit}
echo "ok min-free-space-size (success)"

# metadata object write should succeed even if free space is lower than
# min-free-space value
rm -rf mnt/repo
ostree --repo=mnt/repo init --mode=bare-user
echo 'fsync=false' >> mnt/repo/config
echo 'min-free-space-size=10MB' >> mnt/repo/config
ostree --repo=mnt/repo pull-local --commit-metadata-only /ostree/repo ${host_commit}
echo "ok metadata write even when free space is lower than min-free-space value"

rm -rf mnt/repo

# Test min-free-space-size on deltas

#helper function copied from test-delta.sh
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

mkdir mnt/repo1 mnt/repo2
ostree --repo=mnt/repo1 init --mode=bare-user
ostree --repo=mnt/repo2 init --mode=bare-user
mkdir files
echo "hello" >> files/test.txt
truncate -s 2MB files/test.txt

host_commit=$(ostree --repo=mnt/repo1 commit -b test -s test --tree=dir=files)

origrev=$(ostree --repo=mnt/repo1 rev-parse test)
ostree --repo=mnt/repo1 static-delta generate --empty --to=${origrev}
echo 'fsync=false' >> mnt/repo2/config
echo 'min-free-space-size=14MB' >> mnt/repo2/config

deltaprefix=$(get_assert_one_direntry_matching mnt/repo1/deltas '.')
deltadir=$(get_assert_one_direntry_matching mnt/repo1/deltas/${deltaprefix} '.')

# Try to pull delta and trigger error
if ostree --repo=mnt/repo2 static-delta apply-offline mnt/repo1/deltas/${deltaprefix}/${deltadir} 2>err.txt; then
    fatal "succeeded in doing a delta pull with no free space"
fi
assert_file_has_content err.txt "min-free-space-size"
echo "OK min-free-space-size delta pull (error)"

# Re-adjust min-free-space-size so that delta pull succeeds
sed -i s/min-free-space-size=14MB/min-free-space-size=1MB/g mnt/repo2/config
rm -rf mnt/repo2/deltas
ostree --repo=mnt/repo2 static-delta apply-offline mnt/repo1/deltas/${deltaprefix}/${deltadir}

echo "OK min-free-space-size delta pull (success)"
rm -rf files

umount mnt
losetup -d ${blkdev}
rm testblk.img

date
