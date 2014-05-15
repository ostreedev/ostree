#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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

echo "1..3"

ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo
ostree --repo=sysroot/ostree/repo pull testos testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_not_has_file sysroot/ostree/deploy/testos/current/usr/include/foo.h
if ostree admin --sysroot=sysroot switch --os=testos testos/buildmaster/x86_64-runtime; then
    assert_not_reached "Switch to same ref unexpectedly succeeded"
fi
echo "ok switch expected error"

ostree admin --sysroot=sysroot switch --os=testos testos/buildmaster/x86_64-devel
assert_file_has_content sysroot/ostree/deploy/testos/current/usr/include/foo.h 'header'

echo "ok switch"

ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false anothertestos file://$(pwd)/testos-repo
ostree admin --sysroot=sysroot switch --os=testos anothertestos:testos/buildmaster/x86_64-devel
# Ok this is lame, need a better shell command to extract config, or switch to gjs
ostree admin --sysroot=sysroot status > status.txt
assert_file_has_content status.txt anothertestos

echo "ok switch remotes"

ostree admin --sysroot=sysroot switch --os=testos testos:

echo "ok switch remote only"
