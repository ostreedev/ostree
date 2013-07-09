#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

setup_os_repository "archive-z2"

echo "ok setup"

echo "1..2"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

echo "ok deploy command"

# Commit + upgrade twice, so that we'll rotate out the original deployment
orig_bootcsum=${bootcsum}
os_repository_new_commit
ostree --repo=sysroot/ostree/repo remote add testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot upgrade --os=testos
os_repository_new_commit
ostree --repo=sysroot/ostree/repo remote add testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot upgrade --os=testos

rev=${newrev}
newrev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_not_streq ${orig_bootcsum} ${bootcsum}
assert_not_has_dir sysroot/boot/ostree/testos-${orig_bootcsum}
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'

echo "ok deploy and GC /boot"
