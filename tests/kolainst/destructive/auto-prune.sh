#!/bin/bash
set -xeuo pipefail

# https://github.com/ostreedev/ostree/issues/2670

. ${KOLA_EXT_DATA}/libinsttest.sh

# Log different phases
step=0
increment_step() {
    echo "ok step $step"
    step=$((step+1))
}

journal_cursor() {
    journalctl -o json -n 1 | jq -r '.["__CURSOR"]'
}

assert_journal_grep() {
    local cursor re
    cursor=$1
    shift
    re=$1
    shift

    if ! journalctl -t ostree --after-cursor "${cursor}" --grep="$re" "$@" >/dev/null; then
        fatal "failed to find in journal: $re"; exit 1
    fi
}

assert_not_journal_grep() {
    local cursor re
    cursor=$1
    shift
    re=$1
    shift

    if journalctl -t ostree --after-cursor "${cursor}" --grep="$re" "$@"; then
        fatal "found in journal: $re"; exit 1
    fi
}

one_gb=$((1 * 1024 * 1024 * 1024))

modules_dir=usr/lib/modules/`uname -r`
kernel_size=$(stat -c '%s' ${modules_dir}/vmlinuz)
initramfs_size=$(stat -c '%s' ${modules_dir}/initramfs.img)
bootdata_size=$((${kernel_size} + ${initramfs_size}))
block_size=$(stat --file-system /boot -c '%s')
total_blocks=$(stat --file-system /boot -c '%b')
# If /boot isn't big enough to hold a full 5 kernel+initramfs pairs, then
# we allocate one that is. I chose 5 just to account for overhead crudely.
required_bootdata_size=$((5 * ${bootdata_size}))
if test $((${total_blocks} * ${block_size})) -lt ${required_bootdata_size}; then
    # Make a new loopback-mounted /boot that's large enough
    bootimg=/var/tmp/boot.img
    truncate -s ${required_bootdata_size} ${bootimg}
    # Be sure to mirror existing block size
    mkfs.ext4 -b ${block_size} ${bootimg}
    mount -o loop ${bootimg} /var/mnt
    # Copy existing boot data
    cp -a /boot/* /var/mnt
    umount /var/mnt
     # holds a ref to /boot
    systemctl stop rpm-ostreed
    # Unmount it lazily anyways
    umount -l /boot
    # And put it in place
    mount -o loop ${bootimg} /boot
fi

cat <<EOF
blocks:
  vmlinuz: $((${kernel_size} / ${block_size} + 1))
  initramfs: $((${initramfs_size} / ${block_size} + 1))
EOF

# make two fake ostree commits with modified kernels of about the same size
cd /root
mkdir -p rootfs/"${modules_dir}"
cp /${modules_dir}/vmlinuz rootfs/${modules_dir}
dd if=/dev/urandom of=rootfs/${modules_dir}/vmlinuz count=1 conv=notrunc status=none
ostree commit --base "${host_commit}" -P --tree=dir=rootfs -b modkernel1
dd if=/dev/urandom of=rootfs/${modules_dir}/vmlinuz count=1 conv=notrunc status=none
ostree commit --base "${host_commit}" -P --tree=dir=rootfs -b modkernel2

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
    local free_blocks
    free_blocks=${1:-$(stat --file-system /boot -c '%a')}
    # leave 1 block free
    df /boot
    unshare -m bash -c \
      "mount -o rw,remount /boot && \
       dd if=/dev/zero of=/boot/bigfile count=$((free_blocks-1)) bs=${block_size}"
    df /boot
}

unconsume_bootfs_space() {
    unshare -m bash -c "mount -o rw,remount /boot && rm /boot/bigfile"
    df /boot
}

assert_bootfs_has_n_bootcsum_dirs 1

# first, deploy our second deployment on a filled up bootfs
# the booted deployment is never pruned, so this is a hopeless case and auto-pruning can't save us
consume_bootfs_space
rpm-ostree rebase :modkernel1
cursor=$(journal_cursor)
rm -vf err.txt
if ostree admin finalize-staged 2>err.txt; then
    assert_not_reached "successfully wrote to filled up bootfs"
fi
assert_journal_grep "$cursor" "Disabling auto-prune optimization; insufficient space left in bootfs"
assert_file_has_content err.txt "No space left on device"
unconsume_bootfs_space
rpm-ostree cleanup -bpr
increment_step

# OK, now deploy our second deployment for realsies on a bootfs with ample space
# and sanity-check that auto-pruning doesn't kick in
assert_bootfs_has_n_bootcsum_dirs 1

rpm-ostree rebase :modkernel1
cursor=$(journal_cursor)
ostree admin finalize-staged
assert_not_journal_grep "$cursor" "updating bootloader in two steps"
increment_step

# and put it in rollback position; this is the deployment that'll get auto-pruned
rpm-ostree rollback

assert_bootfs_has_n_bootcsum_dirs 2
bootloader_orig=$(sha256sum /boot/loader/entries/*)

# now try to deploy a third deployment without early pruning; we should hit ENOSPC
consume_bootfs_space
rpm-ostree rebase :modkernel2
cursor=$(journal_cursor)
rm -vf err.txt
if OSTREE_SYSROOT_OPTS=no-early-prune ostree admin finalize-staged 2>err.txt; then
    assert_not_reached "successfully wrote kernel without auto-pruning"
fi
assert_file_has_content err.txt "No space left on device"
increment_step

# there's 3 bootcsums now because it'll also have the partially written
# bootcsum dir we were creating when we hit ENOSPC; this verifies that all the
# deployments have different bootcsums
assert_bootfs_has_n_bootcsum_dirs 3
# but the bootloader wasn't updated
assert_streq "$bootloader_orig" "$(sha256sum /boot/loader/entries/*)"

# now, try again but with auto-pruning enabled
rpm-ostree rebase :modkernel2
cursor=$(journal_cursor)
ostree admin finalize-staged
assert_journal_grep "$cursor" "updating bootloader in two steps"

assert_bootfs_has_n_bootcsum_dirs 2
assert_not_streq "$bootloader_orig" "$(sha256sum /boot/loader/entries/*)"
increment_step

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
# https://github.com/ostreedev/ostree/pull/2866

unconsume_bootfs_space

# Size the bigfile just right so that the kernel+initrd will be just at the max
# limit according to f_bfree.
unshare -m bash -c \
  "mount -o rw,remount /boot && \
   cp /${modules_dir}/{vmlinuz,initramfs.img} /boot"
free_blocks_kernel_and_initrd=$(stat --file-system /boot -c '%f')
unshare -m bash -c \
  "mount -o rw,remount /boot && rm /boot/{vmlinuz,initramfs.img}"
consume_bootfs_space "$((free_blocks_kernel_and_initrd))"

rpm-ostree rebase :modkernel1
cursor=$(journal_cursor)
# Disable auto-pruning to verify we reproduce the bug
rm -vf err.txt
if OSTREE_SYSROOT_OPTS=no-early-prune ostree admin finalize-staged 2>err.txt; then
    assert_not_reached "successfully wrote kernel without auto-pruning"
fi
assert_file_has_content err.txt "No space left on device" 
increment_step

# now, try again but with (now default) auto-pruning enabled
rpm-ostree rebase :modkernel1
cursor=$(journal_cursor)
ostree admin finalize-staged
assert_journal_grep "$cursor" "updating bootloader in two steps"

# Below, we test that the size estimator is blocksize aware. This catches the
# case where the dtb contains many small files such that there's a lot of wasted
# block space we need to account for.
# https://github.com/coreos/fedora-coreos-tracker/issues/1637

unconsume_bootfs_space

mkdir -p rootfs/${modules_dir}/dtb
dtbcount=10000
(set +x; for i in {1..${dtbcount}}; do echo -n x > rootfs/${modules_dir}/dtb/$i; done)
ostree commit --base modkernel1 -P --tree=dir=rootfs -b modkernel3

# a naive estimator would think all those files just take 10000 bytes
dtb_naive_space=$((${dtbcount} / ${block_size}))
consume_bootfs_space "$((free_blocks_kernel_and_initrd - ${dtb_naive_space}))"

rpm-ostree rebase :modkernel3
cursor=$(journal_cursor)
# Disable auto-pruning to verify we reproduce the bug
rm -vf err.txt
if OSTREE_SYSROOT_OPTS=no-early-prune ostree admin finalize-staged 2>err.txt; then
    assert_not_reached "successfully wrote kernel without auto-pruning"
fi
assert_file_has_content err.txt "No space left on device" 
increment_step

# now, try again but with (now default) auto-pruning enabled
rpm-ostree rebase :modkernel3
cursor=$(journal_cursor)
ostree admin finalize-staged
assert_journal_grep "$cursor" "updating bootloader in two steps"
increment_step

echo "ok bootfs auto-prune"
