#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
# Copyright (C) 2014 Owen Taylor <otaylor@redhat.com>
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

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..6"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Check we generate kargs from the kargs.d configs from the first deployment.
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'ostree-kargs-generated-from-config.*true'

rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"
etc=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc

${CMD_PREFIX} ostree admin instutil set-kargs FOO=BAR
${CMD_PREFIX} ostree admin instutil set-kargs FOO=BAZ FOO=BIF TESTARG=TESTVALUE KEYWORD EMPTYLIST=
assert_not_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=BAZ .*FOO=BIF'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*TESTARG=TESTVALUE KEYWORD EMPTYLIST='

# Check that the configured kargs flags and snippet were written.
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'ostree-kargs-generated-from-config.*true'
assert_file_has_content ${etc}/ostree/kargs.d/4000_ostree_instutil 'FOO=BAZ .*FOO=BIF.*TESTARG=TESTVALUE KEYWORD EMPTYLIST='

echo "ok instutil set-kargs (basic)"

${CMD_PREFIX} ostree admin instutil set-kargs --merge FOO=BAR
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=BAZ .*FOO=BIF .*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*TESTARG=TESTVALUE KEYWORD EMPTYLIST='
echo "ok instutil set-kargs --merge"

${CMD_PREFIX} ostree admin instutil set-kargs --merge --replace=FOO=XXX
assert_not_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=XXX'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*TESTARG=TESTVALUE KEYWORD EMPTYLIST='
echo "ok instutil set-kargs --replace"

${CMD_PREFIX} ostree admin instutil set-kargs --merge --append=FOO=BAR --append=APPENDARG=VALAPPEND --append=APPENDARG=2NDAPPEND testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*FOO=XXX.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'options.*APPENDARG=VALAPPEND .*APPENDARG=2NDAPPEND'
echo "ok instutil set-kargs --append"

${CMD_PREFIX} ostree admin instutil set-kargs --import-proc-cmdline
for arg in $(cat /proc/cmdline); do
    case "$arg" in
	ostree=*) # Skip ostree arg that gets stripped out
	   ;;
	initrd=*|BOOT_IMAGE=*) # Skip options set by bootloader that gets filtered out
	   ;;
	*) assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf "options.*$arg"
	   ;;
    esac
done
echo "ok instutil set-kargs --import-proc-cmdline"

# Configure kargs stored in the ostree commit.
mkdir -p osdata/usr/lib/ostree-boot/kargs.d
os_tree_write_file "usr/lib/ostree-boot/kargs.d/4000_FOO" "FOO=USR_1"
os_tree_write_file "usr/lib/ostree-boot/kargs.d/4001_FOO2" "FOO2=USR_2"
os_repository_new_commit

# Upgrade to tree with newly-committed kargs files.
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
# Sanity check a new boot directory was created after upgrading.
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*FOO=USR_1.*FOO2=USR_2'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"
etc=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc

# Check that set-kargs overrides any existing default kargs in /usr/lib/ostree-boot/kargs.d.
${CMD_PREFIX} ostree admin instutil set-kargs FOO=BAR

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*FOO=BAR'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO=USR_1'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO2=USR_2'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'
assert_file_has_content ${etc}/ostree/kargs.d/4000_ostree_instutil 'FOO=BAR'

echo "ok instutil set-kargs default kargs"
