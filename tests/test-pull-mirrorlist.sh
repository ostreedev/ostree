#!/bin/bash
#
# Copyright (C) 2016 Red Hat, Inc.
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

echo "1..3"

setup_fake_remote_repo1 "archive"

setup_mirror () {
  name=$1; shift

  cd ${test_tmpdir}
  mkdir $name
  cd $name
  cp -a ${test_tmpdir}/ostree-srv ostree

  ${OSTREE_HTTPD} --autoexit --daemonize \
    -p ${test_tmpdir}/${name}-port
  port=$(cat ${test_tmpdir}/${name}-port)
  echo "http://127.0.0.1:${port}" > ${test_tmpdir}/${name}-address
}

setup_mirror content_mirror1
setup_mirror content_mirror2
setup_mirror content_mirror3

# Let's delete a file from 1 so that it falls back on 2
cd ${test_tmpdir}/content_mirror1/ostree/gnomerepo
filez=$(find objects/ -name '*.filez' | head -n 1)
rm ${filez}

# Let's delete a file from 1 and 2 so that it falls back on 3
cd ${test_tmpdir}/content_mirror1/ostree/gnomerepo
filez=$(find objects/ -name '*.filez' | head -n 1)
rm ${filez}
cd ${test_tmpdir}/content_mirror2/ostree/gnomerepo
rm ${filez}

# OK, let's just shove the mirrorlist in the first httpd
cat > ${test_tmpdir}/ostree-srv/mirrorlist <<EOF

# comment with empty lines around

http://example.com/nonexistent

$(cat ${test_tmpdir}/content_mirror1-address)/ostree/gnomerepo
$(cat ${test_tmpdir}/content_mirror2-address)/ostree/gnomerepo
$(cat ${test_tmpdir}/content_mirror3-address)/ostree/gnomerepo

EOF

# first let's try just url

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin --no-gpg-verify \
  mirrorlist=$(cat httpd-address)/ostree/mirrorlist
${CMD_PREFIX} ostree --repo=repo pull origin:main

echo "ok pull objects from mirrorlist"

# now let's try contenturl only mirrorlist

cd ${test_tmpdir}
rm -rf repo
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin --no-gpg-verify \
  --contenturl=mirrorlist=$(cat httpd-address)/ostree/mirrorlist \
  $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin:main

echo "ok pull objects from contenturl mirrorlist"

# both

cd ${test_tmpdir}
rm -rf repo
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add origin --no-gpg-verify \
  --contenturl=mirrorlist=$(cat httpd-address)/ostree/mirrorlist \
  mirrorlist=$(cat httpd-address)/ostree/mirrorlist
${CMD_PREFIX} ostree --repo=repo pull origin:main

echo "ok pull objects from split urls mirrorlists"
