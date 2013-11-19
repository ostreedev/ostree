#!/bin/bash
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

echo "1..1"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

# Ok, let's create a long directory chain with custom permissions
etc=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc
mkdir -p ${etc}/a/long/dir/chain
mkdir -p ${etc}/a/long/dir/forking
chmod 700 ${etc}/a
chmod 770 ${etc}/a/long
chmod 777 ${etc}/a/long/dir
chmod 707 ${etc}/a/long/dir/chain
chmod 700 ${etc}/a/long/dir/forking

# Now deploy a new commit
os_repository_new_commit
ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot upgrade --os=testos
newrev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "newrev=${newrev}"

newetc=sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc
assert_file_has_mode() {
  stat -c '%a' $1 > mode.txt
  if ! grep -q -e "$2" mode.txt; then 
      echo 1>&2 "file $1 has mode $(cat mode.txt); expected $2";
      exit 1
  fi
  rm -f mode.txt
}
assert_file_has_mode ${newetc}/a 700
assert_file_has_mode ${newetc}/a/long 770
assert_file_has_mode ${newetc}/a/long/dir 777
assert_file_has_mode ${newetc}/a/long/dir/chain 707
assert_file_has_mode ${newetc}/a/long/dir/forking 700
assert_file_has_mode ${newetc}/a/long/dir 777

echo "ok"
