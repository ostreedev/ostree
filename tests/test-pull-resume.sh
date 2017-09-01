#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
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

setup_fake_remote_repo1 "archive" "" "--force-range-requests"

echo '1..1'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

cd ${test_tmpdir}
rm repo -rf
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo

maxtries=`find ${repopath}/objects | wc -l`
maxtries=`expr $maxtries \* 2`

for ((i = 0; i < $maxtries; i=i+1))
do
  if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>err.log; then
    break
  fi
  assert_file_has_content err.log 'error:.*\(Download incomplete\)\|\(Transferred a partial file\)'
done
if ${CMD_PREFIX} ostree --repo=repo fsck; then
    echo "ok, pull succeeded!"
else
    assert_not_reached "pull failed!"
fi
rm -rf ${repopath}
cp -a ${repopath}.orig ${repopath}
