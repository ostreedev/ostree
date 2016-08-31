#!/bin/bash -ex

this_script="${BASH_SOURCE:-$(readlink -f "$0")}"

setup_bootfs() {
	mkdir -p "$1/proc" "$1/bin"

	# We need the real /proc mounted here so musl's realpath will work, but we
	# want to be able to override /proc/cmdline, so bind mount.
	mount -t proc proc "$1/proc"

	echo "quiet ostree=/ostree/boot.0 ro" >"$1/override_cmdline"
	mount --bind "$1/override_cmdline" "$1/proc/cmdline"

	touch "$1/this_is_bootfs"
	cp "$(dirname "$this_script")/../ostree-prepare-root" "$1/bin"
}

setup_rootfs() {
	mkdir -p "$1/ostree/deploy/linux/deploy/1334/sysroot" \
	         "$1/ostree/deploy/linux/deploy/1334/var" \
	         "$1/ostree/deploy/linux/deploy/1334/usr" \
	         "$1/ostree/deploy/linux/var" \
	         "$1/bin"
	ln -s "deploy/linux/deploy/1334" "$1/ostree/boot.0"
	ln -s . "$1/sysroot"
	touch "$1/ostree/deploy/linux/deploy/1334/this_is_ostree_root" \
	      "$1/ostree/deploy/linux/var/this_is_ostree_var" \
	      "$1/ostree/deploy/linux/deploy/1334/usr/this_is_ostree_usr" \
	      "$1/this_is_real_root"
	cp /bin/busybox "$1/bin"
	busybox --list | xargs -n1 -I '{}' ln -s busybox "$1/bin/{}"
	cp -r "$1/bin" "$1/ostree/deploy/linux/deploy/1334/"
}

setup_overlay() {
	mkdir -p "$1/ostree/deploy/linux/deploy/1334/.usr-ovl-work" \
	         "$1/ostree/deploy/linux/deploy/1334/.usr-ovl-upper"
}

enter_fs() {
	cd "$1"
	mkdir testroot
	pivot_root . testroot
	export PATH=$PATH:/sysroot/bin
	cd /
	umount -l testroot
	rmdir testroot
}

find_in_env() {
	tmpdir="$(mktemp -dt ostree-test-switchroot.XXXXXX)"
	unshare -m <<-EOF
		set -e
		. "$this_script"
		"$1" "$tmpdir"
		enter_fs "$tmpdir"
		ostree-prepare-root /sysroot
		find /
		touch /usr/usr_writable 2>/null \
			&& echo "/usr is writable" \
			|| echo "/usr is not writable"
		touch /sysroot/usr/sysroot_usr_writable 2>/null \
			&& echo "/sysroot/usr is writable" \
			|| echo "/sysroot/usr is not writable"
		EOF
	(cd $tmpdir && find) >permanent_files
	rm -rf "$tmpdir"
}

setup_initrd_env() {
	mount -t tmpfs tmpfs "$1"
	setup_bootfs "$1"
	mkdir "$1/sysroot"
	mount -t tmpfs tmpfs "$1/sysroot"
	setup_rootfs "$1/sysroot"
}

test_that_prepare_root_sets_sysroot_up_correctly_with_initrd() {
	find_in_env setup_initrd_env >files

	grep -qx "/this_is_bootfs" files
	grep -qx "/sysroot/this_is_ostree_root" files
	grep -qx "/sysroot/sysroot/this_is_real_root" files
	grep -qx "/sysroot/var/this_is_ostree_var" files
	grep -qx "/sysroot/usr/this_is_ostree_usr" files

	grep -qx "/sysroot/usr is not writable" files
	echo "ok ostree-prepare-root sets sysroot up correctly with initrd"
}

setup_no_initrd_env() {
	mount --bind "$1" "$1"
	setup_rootfs "$1"
	setup_bootfs "$1"
}

test_that_prepare_root_sets_root_up_correctly_with_no_initrd() {
	find_in_env setup_no_initrd_env >files

	grep -qx "/this_is_ostree_root" files
	grep -qx "/sysroot/this_is_bootfs" files
	grep -qx "/sysroot/this_is_real_root" files
	grep -qx "/var/this_is_ostree_var" files
	grep -qx "/usr/this_is_ostree_usr" files

	grep -qx "/usr is not writable" files
	echo "ok ostree-prepare-root sets root up correctly with no initrd"
}

setup_no_initrd_with_overlay() {
	setup_no_initrd_env "$1"
	setup_overlay "$1"
}

test_that_prepare_root_provides_overlay_over_usr_if__usr_ovl_work_exists() {
	find_in_env setup_no_initrd_with_overlay >files

	grep -qx "/usr is writable" files
	grep -qx "./ostree/deploy/linux/deploy/1334/.usr-ovl-upper/usr_writable" permanent_files
	! grep -qx "./ostree/deploy/linux/deploy/1334/usr/usr_writable" permanent_files || exit 1
	echo "ok ostree-prepare-root sets root up correctly with writable usr overlay"
}

# This script sources itself so we only want to run tests if we're the parent:
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
	. $(dirname $0)/libtest.sh
	unshare -m true || \
	    skip "this test needs to set up mount namespaces, rerun as root"

	echo "1..3"
	test_that_prepare_root_sets_sysroot_up_correctly_with_initrd
	test_that_prepare_root_sets_root_up_correctly_with_no_initrd
	test_that_prepare_root_provides_overlay_over_usr_if__usr_ovl_work_exists
fi
