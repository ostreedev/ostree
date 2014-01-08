#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

echo "1..9"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot status

echo "ok deploy command"

assert_not_has_dir sysroot/boot/loader.0
assert_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.1.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-0.conf
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* quiet'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.1/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot status

echo "ok layout"

ostree admin --sysroot=sysroot deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Need a new bootversion, sine we now have two deployments
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.0.1
assert_not_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.1.0
assert_not_has_dir sysroot/ostree/boot.1.1
# Ensure we propagated kernel arguments from previous deployment
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.0/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot status

echo "ok second deploy"

ostree admin --sysroot=sysroot deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Keep the same bootversion
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
# But swap subbootversion
assert_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.0.1
ostree admin --sysroot=sysroot status

echo "ok third deploy (swap)"

ostree admin --sysroot=sysroot os-init otheros

ostree admin --sysroot=sysroot deploy --os=otheros testos/buildmaster/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
assert_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-1.conf
assert_has_file sysroot/boot/loader/entries/ostree-otheros-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/otheros/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot status

echo "ok independent deploy"

ostree admin --sysroot=sysroot deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.2/etc/os-release 'NAME=TestOS'
assert_has_file sysroot/boot/loader/entries/ostree-testos-2.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot status

echo "ok fourth deploy (retain)"

echo "a new local config file" > sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-config-file
rm -r  sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/testdirectory
rm sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/aconfigfile
ln -s /ENOENT sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-broken-symlink
ostree admin --sysroot=sysroot deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime
linktarget=$(readlink sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-broken-symlink)
test "${linktarget}" = /ENOENT
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-config-file 'a new local config file'
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/aconfigfile
ostree admin --sysroot=sysroot status

echo "ok deploy with modified /etc"

os_repository_new_commit
ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
newrev=$(ostree --repo=sysroot/ostree/repo rev-parse testos:testos/buildmaster/x86_64-runtime)
export newrev
assert_not_streq ${rev} ${newrev}

ostree admin --sysroot=sysroot deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
# New files in /usr/etc
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/a-new-default-config-file "a new default config file"
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/new-default-dir/moo "a new default dir and file"
# And persist /etc changes from before
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/aconfigfile
ostree admin --sysroot=sysroot status

echo "ok upgrade bare"

os_repository_new_commit
ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot upgrade --os=testos
origrev=${rev}
rev=${newrev}
newrev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot status

echo "ok upgrade"

assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
ostree admin --sysroot=sysroot undeploy 1
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0

ostree admin --sysroot=sysroot undeploy 0
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${newrev}.0
ostree admin --sysroot=sysroot status

echo "ok undeploy"

if ostree admin --sysroot=sysroot deploy --os=unknown testos:testos/buildmaster/x86_64-runtime; then
    assert_not_reached "Unexpected successful deploy of unknown OS"
fi
echo "ok deploy with unknown OS"
