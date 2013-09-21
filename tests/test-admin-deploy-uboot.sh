#!/bin/bash
#
# Copyright (C) 2013 Javier Martinez <javier.martinez@collabora.co.uk>
#
# Based on tests/test-admin-deploy-1.sh by Colin Walters.
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

set -e

. $(dirname $0)/libtest.sh

echo "1..1"

setup_os_repository "archive-z2" "uboot"

echo "ok setup"

echo "1..2"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=console=ttySO,115200n8 --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy command"

assert_not_has_dir sysroot/boot/loader.0
# Ensure uEnv.txt exists and has proper values
assert_has_file sysroot/boot/loader.1/uEnv.txt
linux=$(grep linux sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
initrd=$(grep initrd sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
options=$(grep options sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2-4)
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "kernel_image=$linux"
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "ramdisk_image=$initrd"
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "bootargs=$options"

echo "ok U-Boot"

ostree admin --sysroot=sysroot deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Need a new bootversion, sine we now have two deployments
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.0.1
assert_not_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.1.0
assert_not_has_dir sysroot/ostree/boot.1.1
# Ensure we propagated kernel arguments from previous deployment
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO console=ttySO,115200n8'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.0/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
# Ensure uEnv.txt has proper values
assert_has_file sysroot/boot/loader.0/uEnv.txt
linux=$(grep linux sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
initrd=$(grep initrd sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
options=$(grep options sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2-4)
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "kernel_image=$linux"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "ramdisk_image=$initrd"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "bootargs=$options"

ostree admin --sysroot=sysroot status

echo "ok second deploy"

ostree admin --sysroot=sysroot deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Keep the same bootversion
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
# But swap subbootversion
assert_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.0.1
# Ensure uEnv.txt has proper values
assert_has_file sysroot/boot/loader.0/uEnv.txt
linux=$(grep linux sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
initrd=$(grep initrd sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
options=$(grep options sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2-4)
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "kernel_image=$linux"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "ramdisk_image=$initrd"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "bootargs=$options"

echo "ok third deploy (swap)"

ostree admin --sysroot=sysroot deploy --os=otheros testos/buildmaster/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
assert_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-1.conf
assert_has_file sysroot/boot/loader/entries/ostree-otheros-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/otheros/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
# Ensure uEnv.txt has proper values
assert_has_file sysroot/boot/loader.1/uEnv.txt
linux=$(grep linux sysroot/boot/loader.1/entries/ostree-otheros-0.conf | cut -d ' ' -f 2)
initrd=$(grep initrd sysroot/boot/loader.1/entries/ostree-otheros-0.conf | cut -d ' ' -f 2)
options=$(grep options sysroot/boot/loader/entries/ostree-otheros-0.conf | cut -d ' ' -f 2-4)
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "kernel_image=$linux"
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "ramdisk_image=$initrd"
assert_file_has_content sysroot/boot/loader.1/uEnv.txt "bootargs=$options"

echo "ok independent deploy"

ostree admin --sysroot=sysroot deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.2/etc/os-release 'NAME=TestOS'
assert_has_file sysroot/boot/loader/entries/ostree-testos-2.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
# Ensure uEnv.txt has proper values
assert_has_file sysroot/boot/loader.0/uEnv.txt
linux=$(grep linux sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
initrd=$(grep initrd sysroot/boot/loader.0/entries/ostree-testos-0.conf | cut -d ' ' -f 2)
options=$(grep options sysroot/boot/loader/entries/ostree-testos-0.conf | cut -d ' ' -f 2-4)
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "kernel_image=$linux"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "ramdisk_image=$initrd"
assert_file_has_content sysroot/boot/loader.0/uEnv.txt "bootargs=$options"

echo "ok fourth deploy (retain)"

