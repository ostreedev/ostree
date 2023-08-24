#!/bin/bash
set -xeuo pipefail

# https://github.com/ostreedev/ostree/issues/2670

. ${KOLA_EXT_DATA}/libinsttest.sh

# make two fake ostree commits with modified kernels of about the same size
cd /root
mkdir -p rootfs/usr/lib/modules/`uname -r`
cp /usr/lib/modules/`uname -r`/vmlinuz rootfs/usr/lib/modules/`uname -r`
dd if=/dev/urandom of=rootfs/usr/lib/modules/`uname -r`/vmlinuz count=1 conv=notrunc status=none
ostree commit --base "${host_refspec}" -P --tree=dir=rootfs -b modkernel1
dd if=/dev/urandom of=rootfs/usr/lib/modules/`uname -r`/vmlinuz count=1 conv=notrunc status=none
ostree commit --base "${host_refspec}" -P --tree=dir=rootfs -b modkernel2

assert_bootfs_has_n_bootcsum_dirs() {
    local expected=$1; shift
    local actual
    actual=$(ls -d /boot/ostree/${host_osname}-* | wc -l)
    if [ "$expected" != "$actual" ]; then
        ls -l /boot/ostree
        assert_not_reached "expected $expected bootcsum dirs, found $actual"
    fi
}

consume_bootfs_space() {
    local free_blocks block_size
    free_blocks=${1:-$(stat --file-system /boot -c '%a')}
    block_size=$(stat --file-system /boot -c '%s')
    # leave 1 block free
    unshare -m bash -c \
      "mount -o rw,remount /boot && \
       dd if=/dev/zero of=/boot/bigfile count=$((free_blocks-1)) bs=${block_size}"
}

unconsume_bootfs_space() {
    unshare -m bash -c "mount -o rw,remount /boot && rm /boot/bigfile"
}

assert_bootfs_has_n_bootcsum_dirs 1

# first, deploy our second deployment on a filled up bootfs
# the booted deployment is never pruned, so this is a hopeless case and auto-pruning can't save us
consume_bootfs_space
rpm-ostree rebase :modkernel1
if ostree admin finalize-staged |& tee out.txt; then
    assert_not_reached "successfully wrote to filled up bootfs"
fi
assert_file_has_content out.txt "Disabling auto-prune optimization; insufficient space left in bootfs"
assert_file_has_content out.txt "No space left on device"
rm out.txt
unconsume_bootfs_space
rpm-ostree cleanup -bpr

# OK, now deploy our second deployment for realsies on a bootfs with ample space
# and sanity-check that auto-pruning doesn't kick in
assert_bootfs_has_n_bootcsum_dirs 1

rpm-ostree rebase :modkernel1
ostree admin finalize-staged |& tee out.txt
assert_not_file_has_content out.txt "updating bootloader in two steps"
rm out.txt

# and put it in rollback position; this is the deployment that'll get auto-pruned
rpm-ostree rollback

assert_bootfs_has_n_bootcsum_dirs 2
bootloader_orig=$(sha256sum /boot/loader/entries/*)

# now try to deploy a third deployment without early pruning; we should hit ENOSPC
consume_bootfs_space
rpm-ostree rebase :modkernel2
if OSTREE_SYSROOT_OPTS=no-early-prune ostree admin finalize-staged |& tee out.txt; then
    assert_not_reached "successfully wrote kernel without auto-pruning"
fi
assert_file_has_content out.txt "No space left on device"
rm out.txt

# there's 3 bootcsums now because it'll also have the partially written
# bootcsum dir we were creating when we hit ENOSPC; this verifies that all the
# deployments have different bootcsums
assert_bootfs_has_n_bootcsum_dirs 3
# but the bootloader wasn't updated
assert_streq "$bootloader_orig" "$(sha256sum /boot/loader/entries/*)"

# now, try again but with auto-pruning enabled
rpm-ostree rebase :modkernel2
ostree admin finalize-staged |& tee out.txt
assert_file_has_content out.txt "updating bootloader in two steps"
rm out.txt

assert_bootfs_has_n_bootcsum_dirs 2
assert_not_streq "$bootloader_orig" "$(sha256sum /boot/loader/entries/*)"

# This next test relies on the fact that FCOS currently uses ext4 for /boot.
# If that ever changes, we can reprovision boot to be ext4.
if [[ $(findmnt -no FSTYPE /boot) != ext4 ]]; then
    assert_not_reached "/boot is not ext4"
fi

# Put modkernel2 in rollback position
rpm-ostree rollback

# Below, we test that a bootcsum dir sized below f_bfree but still large enough
# to not actually fit (because some filesystems like ext4 include reserved
# overhead in their f_bfree count for some reason) will still trigger the auto-
# prune logic.

unconsume_bootfs_space

# Size the bigfile just right so that the kernel+initrd will be just at the max
# limit according to f_bfree.
unshare -m bash -c \
  "mount -o rw,remount /boot && \
   cp /usr/lib/modules/`uname -r`/{vmlinuz,initramfs.img} /boot"
free_blocks=$(stat --file-system /boot -c '%f')
unshare -m bash -c \
  "mount -o rw,remount /boot && rm /boot/{vmlinuz,initramfs.img}"
consume_bootfs_space "$((free_blocks))"

rpm-ostree rebase :modkernel1
# Disable auto-pruning to verify we reproduce the bug
if OSTREE_SYSROOT_OPTS=no-early-prune ostree admin finalize-staged |& tee out.txt; then
    assert_not_reached "successfully wrote kernel without auto-pruning"
fi
assert_file_has_content out.txt "No space left on device"
rm out.txt

# now, try again but with (now default) auto-pruning enabled
rpm-ostree rebase :modkernel1
ostree admin finalize-staged |& tee out.txt
assert_file_has_content out.txt "updating bootloader in two steps"
rm out.txt

echo "ok bootfs auto-prune"
