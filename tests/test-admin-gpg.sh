#!/bin/bash
#
# Copyright (C) 2019 Rafael Fonseca <r4f4rfs@gmail.com>
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
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_os_repository_signed () {
    mode=$1
    shift
    bootmode=$1
    shift
    bootdir=${1:-usr/lib/modules/3.6.0}

    oldpwd=`pwd`
    keyid="472CDAFA"

    cd ${test_tmpdir}
    mkdir testos-repo
    if test -n "$mode"; then
	      ostree_repo_init testos-repo --mode=${mode}
    else
	      ostree_repo_init testos-repo
    fi

    cd ${test_tmpdir}
    mkdir osdata
    cd osdata
    kver=3.6.0
    mkdir -p usr/bin ${bootdir} usr/lib/modules/${kver} usr/share usr/etc
    kernel_path=${bootdir}/vmlinuz
    initramfs_path=${bootdir}/initramfs.img
    # /usr/lib/modules just uses "vmlinuz", since the version is in the module
    # directory name.
    if [[ $bootdir != usr/lib/modules/* ]]; then
        kernel_path=${kernel_path}-${kver}
        initramfs_path=${bootdir}/initramfs-${kver}.img
    fi
    echo "a kernel" > ${kernel_path}
    echo "an initramfs" > ${initramfs_path}
    bootcsum=$(cat ${kernel_path} ${initramfs_path} | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    # Add the checksum for legacy dirs (/boot, /usr/lib/ostree-boot), but not
    # /usr/lib/modules.
    if [[ $bootdir != usr/lib/modules/* ]]; then
        mv ${kernel_path}{,-${bootcsum}}
        mv ${initramfs_path}{,-${bootcsum}}
    fi

    echo "an executable" > usr/bin/sh
    echo "some shared data" > usr/share/langs.txt
    echo "a library" > usr/lib/libfoo.so.0
    ln -s usr/bin bin
cat > usr/etc/os-release <<EOF
NAME=TestOS
VERSION=42
ID=testos
VERSION_ID=42
PRETTY_NAME="TestOS 42"
EOF
    echo "a config file" > usr/etc/aconfigfile
    mkdir -p usr/etc/NetworkManager
    echo "a default daemon file" > usr/etc/NetworkManager/nm.conf
    mkdir -p usr/etc/testdirectory
    echo "a default daemon file" > usr/etc/testdirectory/test

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmain/x86_64-runtime -s "Build" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome

    # Ensure these commits have distinct second timestamps
    sleep 2
    echo "a new executable" > usr/bin/sh
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.10 -b testos/buildmain/x86_64-runtime -s "Build" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome

    cd ${test_tmpdir}
    rm -rf osdata-devel
    mkdir osdata-devel
    tar -C osdata -cf - . | tar -C osdata-devel -xf -
    cd osdata-devel
    mkdir -p usr/include
    echo "a development header" > usr/include/foo.h
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmain/x86_64-devel -s "Build" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo fsck -q

    cd ${test_tmpdir}
    mkdir sysroot
    export OSTREE_SYSROOT=sysroot
    ${CMD_PREFIX} ostree admin init-fs sysroot
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> sysroot/ostree/repo/config
    fi
    ${CMD_PREFIX} ostree admin os-init testos

    case $bootmode in
        "syslinux")
	    setup_os_boot_syslinux
            ;;
        "uboot")
	    setup_os_boot_uboot
            ;;
        *grub2*)
        setup_os_boot_grub2 "${bootmode}"
            ;;
    esac

    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir} ostree
    ${OSTREE_HTTPD} --autoexit --daemonize -p ${test_tmpdir}/httpd-port
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
    cd ${oldpwd}
}

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository_signed "archive" "syslinux"

echo "1..2"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add testos $(cat httpd-address)/ostree/testos-repo
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --gpg-verify=true --remote=testos testos-repo testos/buildmain/x86_64-runtime
# This initial deployment gets kicked off with some kernel arguments
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmain/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy command"

${CMD_PREFIX} ostree admin status > status.txt
test -f status.txt
assert_file_has_content status.txt "GPG: Signature made"
assert_not_file_has_content status.txt "GPG: Can't check signature: public key not found"
echo 'ok gpg signature'
