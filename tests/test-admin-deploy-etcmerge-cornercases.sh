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

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

echo "1..2"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/ostree/testos-${bootcsum}

etc=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc

# modified config file
echo "a modified config file" > ${etc}/NetworkManager/nm.conf

# Ok, let's create a long directory chain with custom permissions
mkdir -p ${etc}/a/long/dir/chain
mkdir -p ${etc}/a/long/dir/forking
chmod 700 ${etc}/a
chmod 770 ${etc}/a/long
chmod 777 ${etc}/a/long/dir
chmod 707 ${etc}/a/long/dir/chain
chmod 700 ${etc}/a/long/dir/forking

# Symlink to nonexistent path, to ensure we aren't walking symlinks
ln -s no-such-file ${etc}/a/link-to-no-such-file

# Remove a directory
rm ${etc}/testdirectory -rf

# Now deploy a new commit
os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
echo "newrev=${newrev}"
newroot=sysroot/ostree/deploy/testos/deploy/${newrev}.0
newetc=${newroot}/etc

assert_file_has_content ${newroot}/usr/etc/NetworkManager/nm.conf "a default daemon file"
assert_file_has_content ${newetc}/NetworkManager/nm.conf "a modified config file"

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

test -L ${newetc}/a/link-to-no-such-file || assert_not_reached "should have symlink"

assert_has_dir ${newroot}/usr/etc/testdirectory
assert_not_has_dir ${newetc}/testdirectory

echo "ok"

# Add /etc/initially-empty
cd "${test_tmpdir}/osdata"
mkdir -p usr/etc/initially-empty
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmaster/x86_64-runtime -s "Add empty directory"
cd ${test_tmpdir}

# Upgrade, check that we have it
${CMD_PREFIX} ostree admin upgrade --os=testos
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_has_dir sysroot/ostree/deploy/testos/deploy/$rev.0/etc/initially-empty

# Now add a two files in initially-empty
cd "${test_tmpdir}/osdata"
touch usr/etc/initially-empty/{afile,bfile}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmaster/x86_64-runtime -s "Add empty directory"

# Upgrade, check that we have the two new files
cd ${test_tmpdir}
${CMD_PREFIX} ostree admin upgrade --os=testos
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_has_file sysroot/ostree/deploy/testos/deploy/$rev.0/etc/initially-empty/afile
assert_has_file sysroot/ostree/deploy/testos/deploy/$rev.0/etc/initially-empty/bfile

# Replace config file with default directory
cd "${test_tmpdir}/osdata"
mkdir usr/etc/somenewdir
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmaster/x86_64-runtime -s "Add default dir"
cd ${test_tmpdir}
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
newconfpath=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/somenewdir
echo "some content blah" > ${newconfpath}
if ${CMD_PREFIX} ostree admin upgrade --os=testos 2>err.txt; then
    assert_not_reached "upgrade should have failed"
fi
assert_file_has_content err.txt "Modified config file newly defaults to directory"
rm ${newconfpath}

# Remove parent directory of modified config file
cd "${test_tmpdir}/osdata"
rm -rf usr/etc/initially-empty
${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit -b testos/buildmaster/x86_64-runtime -s "Remove default dir"
cd ${test_tmpdir}
newconfpath=sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/initially-empty/mynewfile
touch ${newconfpath}
${CMD_PREFIX} ostree admin upgrade --os=testos
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.0/usr/etc/initially-empty
assert_has_file sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/initially-empty/mynewfile
rm ${newconfpath}

echo "ok"
