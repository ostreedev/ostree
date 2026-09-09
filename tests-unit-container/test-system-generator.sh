#!/bin/bash
# Tests ostree-system-generator's boot.mount handling. It expects to run in
# a podman container. See the `unitcontainer` target in the Justfile.
#
# The interesting case is an /etc/fstab entry for /boot: systemd-fstab-generator
# owns boot.mount then, so we must not generate our own and must still generate
# var.mount.

set -xeuo pipefail

# Ensure this isn't run accidentally
test "${TEST_CONTAINER}" = 1

generator=/usr/lib/systemd/system-generators/ostree-system-generator

workdir=$(mktemp -d)
cmdline=${workdir}/cmdline

cleanup() {
	if mountpoint /proc/cmdline &>/dev/null; then
		umount -l /proc/cmdline
	fi
	if test -f /etc/fstab.test-orig; then
		mv /etc/fstab.test-orig /etc/fstab
	else
		rm -f /etc/fstab
	fi
	rm -rf /run/ostree /sysroot/boot "${workdir}"
}
trap cleanup EXIT

# The generator no-ops unless it thinks we booted via ostree
mkdir -p /run/ostree
rm -f /run/ostree/initramfs-mount-var

# Fake a kernel commandline with a valid ostree= bootlink
echo "root=UUID=cafebabe ostree=/ostree/boot.1/default/cafecafe/0" >"${cmdline}"
mount --bind "${cmdline}" /proc/cmdline

# No /var entry in fstab, otherwise var.mount is skipped
if test -f /etc/fstab; then
	mv /etc/fstab /etc/fstab.test-orig
fi
: >/etc/fstab

# /boot on the same partition as /sysroot is detected via this symlink
mkdir -p /boot /sysroot/boot/loader.0
ln -sf loader.0 /sysroot/boot/loader

#
# Case 1: no /boot entry in fstab, we generate boot.mount
#
dest=${workdir}/case1
mkdir -p "${dest}"
${generator} "${dest}" "${dest}" "${dest}"

test -f "${dest}/boot.mount"
grep -q '^What=/sysroot/boot$' "${dest}/boot.mount"
grep -q '^Where=/boot$' "${dest}/boot.mount"
test -L "${dest}/local-fs.target.requires/boot.mount"
test -f "${dest}/var.mount"

echo "ok generated boot.mount"

#
# Case 2: /etc/fstab has an entry for /boot, so systemd-fstab-generator owns
# boot.mount. We must skip it and crucially still generate var.mount.
# Previously we wrote our own unit and aborted with EEXIST, so /var was never
# mounted.
#
echo '/dev/disk/by-uuid/deadbeef /boot ext4 defaults 1 2' >/etc/fstab

dest=${workdir}/case2
mkdir -p "${dest}"
${generator} "${dest}" "${dest}" "${dest}"

test '!' -e "${dest}/boot.mount"
test '!' -e "${dest}/local-fs.target.requires/boot.mount"
test -f "${dest}/var.mount"
grep -q '^What=/sysroot/ostree/deploy/default/var$' "${dest}/var.mount"
test -L "${dest}/local-fs.target.requires/var.mount"

echo "ok fstab /boot entry wins"

# Trailing slashes and duplicate separators must still match
printf '/dev/disk/by-uuid/deadbeef //boot/ ext4 defaults 1 2\n' >/etc/fstab

dest=${workdir}/case2b
mkdir -p "${dest}"
${generator} "${dest}" "${dest}" "${dest}"

test '!' -e "${dest}/boot.mount"
test -f "${dest}/var.mount"

echo "ok fstab /boot entry is path-normalized"

#
# Case 3: no /etc/fstab at all; nothing owns /boot or /var, so we generate both
#
rm -f /etc/fstab

dest=${workdir}/case3
mkdir -p "${dest}"
${generator} "${dest}" "${dest}" "${dest}"

test -f "${dest}/boot.mount"
test -L "${dest}/local-fs.target.requires/boot.mount"
test -f "${dest}/var.mount"

echo "ok missing /etc/fstab is tolerated"

: >/etc/fstab

#
# Case 4: /boot is a separate partition (no /sysroot/boot/loader symlink),
# systemd handles it, so we generate no boot.mount at all.
#
rm -f /sysroot/boot/loader
dest=${workdir}/case4
mkdir -p "${dest}"
${generator} "${dest}" "${dest}" "${dest}"

test '!' -e "${dest}/boot.mount"
test '!' -e "${dest}/local-fs.target.requires/boot.mount"
test -f "${dest}/var.mount"

echo "ok no boot.mount for separate /boot partition"
