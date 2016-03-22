#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive-z2" "syslinux"

echo "1..3"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime

# Override the default and say that /boot is not a partition
env OSTREE_DEBUG_MOUNTINFO_FILE=${test_srcdir}/boot_on_root_no_bind.mountinfo \
  ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "KERNEL /boot/ostree/testos.*vmlinuz"
assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "INITRD /boot/ostree/testos.*initramfs"

echo "ok bootonslash - /boot not bind-mounted"

# Same again, but with /boot bind-mounted from real /
env OSTREE_DEBUG_MOUNTINFO_FILE=${test_srcdir}/boot_on_root_bind_mount.mountinfo \
  ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "KERNEL /boot/ostree/testos.*vmlinuz"
assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "INITRD /boot/ostree/testos.*initramfs"

echo "ok bootonslash - /boot bind-mounted"

${CMD_PREFIX} ostree admin undeploy 0

OSTREE_DEBUG_MOUNTINFO_FILE=${test_srcdir}/boot_partition.mountinfo ${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "KERNEL /ostree/testos.*vmlinuz"
assert_file_has_content sysroot/boot/syslinux/syslinux.cfg "INITRD /ostree/testos.*initramfs"

echo "ok bootpartition"
