#!/bin/bash
#
# Copyright (C) 2019 Robert Fairley <rfairley@redhat.com>
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

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "sysroot.bootloader none"

extra_admin_tests=10
folders_instead_symlinks_in_boot=1

. $(dirname $0)/admin-test.sh

# Content of boot/loader that is not owned by ostree must survive the swap
cd ${test_tmpdir}
echo "timeout 3" > sysroot/boot/loader/loader.conf
echo "a random seed" > sysroot/boot/loader/random-seed
chmod 0600 sysroot/boot/loader/random-seed
chmod 0700 sysroot/boot/loader
mkdir -p sysroot/boot/loader/keys/auto
echo "a key" > sysroot/boot/loader/keys/auto/db.auth
cat > sysroot/boot/loader/entries/otheros.conf << EOF
title Other OS
linux /vmlinuz-otheros
EOF
# Entries of other operating systems may be anything, e.g. dangling symlinks
ln -s ../nonexistent sysroot/boot/loader/entries/dangling.conf
# Change the kernel arguments to force a bootloader configuration swap
${CMD_PREFIX} ostree admin deploy --karg-append=preserve=1 --os=testos testos:testos/buildmain/x86_64-runtime > out.txt
assert_file_has_content out.txt "bootconfig swap: yes"
assert_boot_loader_is_dir
assert_file_has_content sysroot/boot/loader/loader.conf "timeout 3"
assert_file_has_content sysroot/boot/loader/random-seed "a random seed"
assert_streq "$(stat -c '%a' sysroot/boot/loader)" 700
assert_streq "$(stat -c '%a' sysroot/boot/loader/random-seed)" 600
assert_file_has_content sysroot/boot/loader/keys/auto/db.auth "a key"
assert_file_has_content sysroot/boot/loader/entries/otheros.conf "Other OS"
test -L sysroot/boot/loader/entries/dangling.conf
assert_file_has_content_literal sysroot/boot/loader/entries.srel type1
${CMD_PREFIX} ostree admin status
echo "ok loader directory preserves content not owned by ostree"

# An interrupted deployment may leave boot/loader.tmp behind; it must be ignored
# when reading the current state, and replaced by the next deployment.
${CMD_PREFIX} ostree admin status > status-before.txt
mkdir -p sysroot/boot/loader.tmp/entries
echo "garbage" > sysroot/boot/loader.tmp/entries/ostree-1.conf
echo "stale" > sysroot/boot/loader.tmp/stale-file
${CMD_PREFIX} ostree admin status > status-after.txt
diff -u status-before.txt status-after.txt
# Change the kernel arguments to force a bootloader configuration swap
${CMD_PREFIX} ostree admin deploy --karg-append=loadertmp=stale --os=testos testos:testos/buildmain/x86_64-runtime
assert_boot_loader_is_dir
assert_not_has_file sysroot/boot/loader/stale-file
assert_not_file_has_content sysroot/boot/loader/entries/ostree-1.conf "garbage"
if ! grep -q "loadertmp=stale" sysroot/boot/loader/entries/ostree-*.conf; then
    fatal "new deployment not found in boot/loader/entries"
fi
${CMD_PREFIX} ostree admin status
echo "ok stale loader.tmp is ignored"

# Without RENAME_EXCHANGE (e.g. VFAT before Linux 6.0), the swap is done with two
# renames. If interrupted in between, the previous configuration is left in
# boot/loader.old, which must be used until the next deployment.
# Make the deployments identical first, so that the next deployment only swaps
# the bootlinks.
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin status > status-before.txt
mv sysroot/boot/loader sysroot/boot/loader.old
mkdir -p sysroot/boot/loader.tmp/entries
echo "garbage" > sysroot/boot/loader.tmp/entries/ostree-1.conf
${CMD_PREFIX} ostree admin status > status-after.txt
diff -u status-before.txt status-after.txt
# Bootloaders only look at boot/loader, so it must be restored by any deployment,
# including one that only swaps the bootlinks.
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime > out.txt
assert_file_has_content out.txt "bootconfig swap: no"
if test -L sysroot/boot/loader || ! test -d sysroot/boot/loader; then
    fatal "sysroot/boot/loader not restored"
fi
assert_not_has_dir sysroot/boot/loader.old
assert_file_has_content sysroot/boot/loader/loader.conf "timeout 3"
# The same goes for editing the kernel arguments in place
mv sysroot/boot/loader sysroot/boot/loader.old
${CMD_PREFIX} ostree admin kargs edit-in-place --append-if-missing=recovered=1 testos:testos/buildmain/x86_64-runtime
if test -L sysroot/boot/loader || ! test -d sysroot/boot/loader; then
    fatal "sysroot/boot/loader not restored"
fi
assert_not_has_dir sysroot/boot/loader.old
${CMD_PREFIX} ostree admin kargs edit-in-place --append-if-missing=inplace=1 testos:testos/buildmain/x86_64-runtime
for karg in recovered=1 inplace=1; do
    if ! grep -q "${karg}" sysroot/boot/loader/entries/ostree-*.conf; then
        fatal "${karg} not found in boot/loader/entries"
    fi
done
${CMD_PREFIX} ostree admin status
echo "ok interrupted non-atomic swap is recovered"

# Old ostree entries are removed from the staging copy of loader/entries, so it
# must not be shared with the live directory through a symbolic link.
mv sysroot/boot/loader/entries sysroot/boot/shared-entries
ln -s ../shared-entries sysroot/boot/loader/entries
ls sysroot/boot/shared-entries > entries-before.txt
if ${CMD_PREFIX} ostree admin deploy --karg-append=symlink=1 --os=testos testos:testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "deployed with boot/loader/entries as a symbolic link"
fi
assert_file_has_content err.txt "boot/loader/entries is not a directory"
ls sysroot/boot/shared-entries > entries-after.txt
diff -u entries-before.txt entries-after.txt
rm sysroot/boot/loader/entries
mv sysroot/boot/shared-entries sysroot/boot/loader/entries
echo "ok boot/loader/entries as a symbolic link is rejected"

# On a VFAT /boot (e.g. an ESP) without boot/loader yet, the first deployment
# creates boot/loader as a directory.
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "sysroot.bootloader none"
${CMD_PREFIX} ostree pull-local --repo=sysroot/ostree/repo --remote testos testos-repo testos/buildmain/x86_64-runtime
for path in loader loader.tmp; do
    if test -e sysroot/boot/${path} || test -L sysroot/boot/${path}; then
        fatal "sysroot/boot/${path} exists"
    fi
done
orig_sysroot_debug="${OSTREE_SYSROOT_DEBUG}"
export OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},test-boot-vfat"
# The /boot/... paths of sysroot.bootprefix rely on the boot -> . symlink
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootprefix true
if ${CMD_PREFIX} ostree admin deploy --os testos testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "deployed with sysroot.bootprefix on VFAT"
fi
assert_file_has_content err.txt "sysroot.bootprefix is not supported with a VFAT /boot"
for path in loader loader.tmp; do
    if test -e sysroot/boot/${path} || test -L sysroot/boot/${path}; then
        fatal "sysroot/boot/${path} exists"
    fi
done
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootprefix false
echo "ok sysroot.bootprefix rejected on VFAT /boot"
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --os testos testos/buildmain/x86_64-runtime
assert_boot_loader_is_dir
# The conformance marker is up to the bootloader (e.g. bootctl install)
assert_not_has_file sysroot/boot/loader/entries.srel
if test -L sysroot/boot/boot; then
    fatal "sysroot/boot/boot symlink created on VFAT"
fi
assert_file_has_content sysroot/boot/loader/entries/ostree-1.conf 'options.* root=LABEL=MOO'
${CMD_PREFIX} ostree admin deploy --os testos testos/buildmain/x86_64-runtime
assert_boot_loader_is_dir
assert_has_file sysroot/boot/loader/entries/ostree-1.conf
assert_has_file sysroot/boot/loader/entries/ostree-2.conf
${CMD_PREFIX} ostree admin status
export OSTREE_SYSROOT_DEBUG="${orig_sysroot_debug}"
echo "ok first deployment on VFAT /boot"

# Bootloader backends relying on symbolic links are rejected on VFAT
for bootloader in syslinux uboot; do
    cd ${test_tmpdir}
    rm httpd osdata testos-repo sysroot -rf
    setup_os_repository "archive" "sysroot.bootloader ${bootloader}"
    ${CMD_PREFIX} ostree pull-local --repo=sysroot/ostree/repo --remote testos testos-repo testos/buildmain/x86_64-runtime
    if env OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},test-boot-vfat" \
           ${CMD_PREFIX} ostree admin deploy --os testos testos/buildmain/x86_64-runtime 2>err.txt; then
        fatal "deployed with ${bootloader} on VFAT"
    fi
    assert_file_has_content err.txt "not supported with a VFAT /boot"
done
echo "ok syslinux and U-Boot rejected on VFAT /boot"

# entries.srel declaring type1 semantics requires boot/loader to be a directory
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "sysroot.bootloader none"
${CMD_PREFIX} ostree pull-local --repo=sysroot/ostree/repo --remote testos testos-repo testos/buildmain/x86_64-runtime
mkdir -p sysroot/boot/loader.0
ln -s loader.0 sysroot/boot/loader
echo type1 > sysroot/boot/loader/entries.srel
if ${CMD_PREFIX} ostree admin deploy --os testos testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "deployed with type1 entries.srel and boot/loader symlink"
fi
assert_file_has_content err.txt "entries.srel declares type1 semantics"
# Other semantics are not ours to write entries for
rm sysroot/boot/loader
mv sysroot/boot/loader.0 sysroot/boot/loader
echo type2 > sysroot/boot/loader/entries.srel
if ${CMD_PREFIX} ostree admin deploy --os testos testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "deployed with type2 entries.srel"
fi
assert_file_has_content err.txt "entries.srel declares unsupported semantics: type2"
echo "ok type1 entries.srel with boot/loader symlink"

# Test that the bootloader configuration "none" generates BLS config snippets.
cd ${test_tmpdir}
rm httpd osdata testos-repo sysroot -rf
setup_os_repository "archive" "sysroot.bootloader none"
setup_os_boot_loader_dir
${CMD_PREFIX} ostree pull-local --repo=sysroot/ostree/repo --remote testos testos-repo testos/buildmain/x86_64-runtime
# Test that configuring sysroot.bootloader="none" is a workaround for previous
# grub2 bootloader issue (see https://github.com/ostreedev/ostree/issues/1774)
mkdir -p sysroot/boot/grub2
touch sysroot/boot/grub2/grub.cfg
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os testos testos/buildmain/x86_64-runtime > out.txt
assert_file_has_content out.txt "Bootloader updated.*"
assert_file_has_content sysroot/boot/loader/entries/ostree-1.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/.vmlinuz-3.6.0.hmac  'an hmac file'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/initramfs-3.6.0.img 'an initramfs'
assert_boot_loader_is_dir
echo "ok generate bls config on first deployment"

# Without RENAME_EXCHANGE (e.g. VFAT before Linux 6.0), the swap falls back to two
# renames, and must leave neither loader.tmp nor loader.old behind.
env OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},test-no-rename-exchange" \
    ${CMD_PREFIX} ostree admin deploy --karg-append=fallback=1 --os testos testos/buildmain/x86_64-runtime > out.txt
assert_file_has_content out.txt "bootconfig swap: yes"
assert_boot_loader_is_dir
if ! grep -q "fallback=1" sysroot/boot/loader/entries/ostree-*.conf; then
    fatal "new deployment not found in boot/loader/entries"
fi
${CMD_PREFIX} ostree admin status
echo "ok swap without RENAME_EXCHANGE"

# TODO: add tests to try setting with an unsupported bootloader config,
# once https://github.com/ostreedev/ostree/issues/1827 is solved.
# The tests should check that the following commands fail:
#  ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootloader unsupported_bootloader
#  ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set sysroot.bootloader ""
