#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

echo "1..2"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
echo "unconfigured-state=Use \"subscription-manager\" to enable online updates for example.com OS" >> sysroot/ostree/deploy/testos/deploy/${rev}.0.origin

ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
if ostree admin --sysroot=sysroot upgrade --os=testos 2>err.txt; then
    assert_not_reached "upgrade unexpectedly succeeded"
fi
assert_file_has_content err.txt "Use.*subscription.*online"

echo "ok error"

ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false otheros file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot switch --os=testos otheros:testos/buildmaster/x86_64-runtime

echo "ok switch"
