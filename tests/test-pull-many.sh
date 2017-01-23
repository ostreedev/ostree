#!/bin/bash
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

setup_fake_remote_repo1 "archive-z2"
cd ${test_tmpdir}
rm ostree-srv/gnomerepo/ -rf
mkdir ostree-srv/gnomerepo/
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo init --mode=archive

echo '1..1'

# Simulate a large archive pull from scratch.  Large here is
# something like the Fedora Atomic Workstation branch which has
# objects: meta: 7497 content: 103541
# 9443 directories, 7097 symlinks, 112832 regfiles
# So we'll make ~11 files per dir, with one of them a symlink

cd ${test_tmpdir}
rm main -rf
mkdir main
cd main
ndirs=9443
depth=0
echo "$(date): Generating content..."
set +x  # No need to spam the logs for this
while [ $ndirs -gt 0 ]; do
    # 2/3 of the time, recurse a dir, up to a max of 9, otherwise back up
    x=$(($ndirs % 3))
    case $x in
        0) if [ $depth -gt 0 ]; then cd ..; depth=$((depth-1)); fi ;;
        1|2) if [ $depth -lt 9 ]; then
                 mkdir dir-${ndirs}
                 cd dir-${ndirs}
                 depth=$((depth+1))
             else
                 if [ $depth -gt 0 ]; then cd ..; depth=$((depth-1)); fi
             fi ;;
    esac

    # One symlink - we use somewhat predictable content to have dupes
    ln -s $(($x % 20)) link-$ndirs
    # 10 files
    nfiles=10
    while [ $nfiles -gt 0 ]; do
        echo file-$ndirs-$nfiles > f$ndirs-$nfiles
        nfiles=$((nfiles-1))
    done
    ndirs=$((ndirs-1))
done
set -x
cd ${test_tmpdir}
mkdir build-repo
${CMD_PREFIX} ostree --repo=build-repo init --mode=bare-user
echo "$(date): Committing content..."
${CMD_PREFIX} ostree --repo=build-repo commit -b main -s 'big!' --tree=dir=main
for x in commit dirtree dirmeta file; do
    find build-repo/objects -name '*.'${x} |wc -l > ${x}count
    echo "$x: " $(cat ${x}count)
done
assert_file_has_content commitcount '^1$'
assert_file_has_content dirmetacount '^1$'
assert_file_has_content filecount '^94433$'
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo pull-local build-repo

echo "$(date): Pulling content..."
rm repo -rf
${CMD_PREFIX} ostree --repo=repo init --mode=archive
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

${CMD_PREFIX} ostree --repo=repo pull --mirror origin main
${CMD_PREFIX} ostree --repo=repo fsck
for x in commit dirtree dirmeta filez; do
    find repo/objects -name '*.'${x} |wc -l > ${x}count
    echo "$x: " $(cat ${x}count)
done
assert_file_has_content commitcount '^1$'
assert_file_has_content dirmetacount '^1$'
assert_file_has_content filezcount '^94433$'

echo "ok"
