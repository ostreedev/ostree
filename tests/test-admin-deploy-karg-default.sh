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
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..7"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
# Check we generate kargs from the kargs.d configs from the first deployment.
assert_file_has_content sysroot/boot/loader/entries/ostree-1-testos.conf 'ostree-kargs-generated-from-config.*true'

initial_rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "initial_rev=${initial_rev}"

# Configure kargs stored in the ostree commit.
mkdir -p osdata/usr/lib/ostree-boot/kargs.d
os_tree_write_file "usr/lib/ostree-boot/kargs.d/4000_FOO" "FOO=USR_1"
os_tree_write_file "usr/lib/ostree-boot/kargs.d/4001_FOO2" "FOO2=USR_2"
os_repository_commit "testos-repo"

# Upgrade to tree with newly-committed kargs files.
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
# Sanity check a new boot directory was created after upgrading.
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*FOO=USR_1.*FOO2=USR_2'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs base config"

# Configure kargs stored in the default configuration (/usr/etc).
mkdir -p osdata/usr/etc/ostree/kargs.d
os_tree_write_file "usr/etc/ostree/kargs.d/8000_MOO" "MOO=ETC_USR_1"
os_tree_write_file "usr/etc/ostree/kargs.d/8001_MOO2" "MOO2=ETC_USR_2"
os_repository_commit "testos-repo"

${CMD_PREFIX} ostree admin upgrade --os=testos
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*FOO=USR_1.*FOO2=USR_2.*MOO=ETC_USR_1.*MOO2=ETC_USR_2'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs default config"

# Configure kargs through the host config file.
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"
etc=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc
assert_has_dir ${etc}
mkdir -p ${etc}/ostree/kargs.d
# Configure a new karg (append).
echo "HELLO=ETC_1" > ${etc}/ostree/kargs.d/2000_HELLO
# Overwrite existing karg from /usr/etc/ostree/kargs.d (replace).
echo "MOO=ETC_2" > ${etc}/ostree/kargs.d/8000_MOO
# Overwrite existing karg from /usr/lib/ostree-boot/kargs.d (replace).
echo "FOO=ETC_3" > ${etc}/ostree/kargs.d/4000_FOO

# Re-deploy with host-configured kernel args.
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*HELLO=ETC_1.*FOO=ETC_3.*FOO2=USR_2.*MOO=ETC_2.*MOO2=ETC_USR_2'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'MOO=ETC_USR_1'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO=USR_1'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs host config"

rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"
etc=sysroot/ostree/deploy/testos/deploy/${rev}.1/etc
mkdir -p ${etc}/ostree/kargs.d
# Clear base kargs by writing an empty file which overrides them (delete).
echo "" > ${etc}/ostree/kargs.d/8000_MOO
echo "" > ${etc}/ostree/kargs.d/4000_FOO

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*HELLO=ETC_1.*FOO2=USR_2.*MOO2=ETC_USR_2'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'MOO\>'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO\>'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs delete empty file"

rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "rev=${rev}"
etc=sysroot/ostree/deploy/testos/deploy/${rev}.2/etc
mkdir -p ${etc}/ostree/kargs.d
rm ${etc}/ostree/kargs.d/8000_MOO
rm ${etc}/ostree/kargs.d/4000_FOO
# Clear base kargs by symlinking to /dev/null.
ln -s /dev/null ${etc}/ostree/kargs.d/8000_MOO
ln -s /dev/null ${etc}/ostree/kargs.d/4000_FOO

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime

assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*HELLO=ETC_1.*FOO2=USR_2.*MOO2=ETC_USR_2'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'MOO\>'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO\>'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs delete /dev/null"

${CMD_PREFIX} ostree admin upgrade --os=testos --allow-downgrade --override-commit=${initial_rev}

# Only the config in /etc/ostree/kargs.d remains.
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*HELLO=ETC_1'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'MOO=ETC_USR_1'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'MOO2=ETC_USR_2'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO=USR_1'
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'FOO2=USR_2'
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs downgrade"

${CMD_PREFIX} ostree admin deploy --os=testos --karg-append=TESTARG=TESTVALUE testos:testos/buildmaster/x86_64-runtime

# Check we carry over previous deployment kargs, after passing in a kargs
# override.
assert_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'options.*HELLO=ETC_1.*TESTARG=TESTVALUE'
# Check we won't regenerate from the config again.
assert_not_file_has_content sysroot/boot/loader/entries/ostree-2-testos.conf 'ostree-kargs-generated-from-config.*true'

echo "ok default kargs overridden"
