#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

setup_fake_remote_repo1 "archive"

echo '1..9'

# And another web server acting as the metalink server
cd ${test_tmpdir}
mkdir metalink-data
cd metalink-data
${OSTREE_HTTPD} --autoexit --daemonize -p ${test_tmpdir}/metalink-httpd-port
metalink_port=$(cat ${test_tmpdir}/metalink-httpd-port)
echo "http://127.0.0.1:${metalink_port}" > ${test_tmpdir}/metalink-httpd-address

${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u

summary_path=${test_tmpdir}/ostree-srv/gnomerepo/summary

echo -n broken > ${summary_path}.bad

cd ${test_tmpdir}

cat > metalink-valid-summary.xml <<EOF
    <file name="summary">
      <size>$(stat -c '%s' ${summary_path})</size>
      <verification>
        <hash type="md5">$(md5sum ${summary_path} | cut -f 1 -d ' ')</hash>
        <hash type="sha256">$(sha256sum ${summary_path} | cut -f 1 -d ' ')</hash>
        <hash type="sha512">$(sha512sum ${summary_path} | cut -f 1 -d ' ')</hash>
      </verification>
      <resources maxconnections="1">
        <url protocol="http" type="http" location="US" preference="100" >$(cat httpd-address)/ostree/gnomerepo/summary.bad</url>
        <url protocol="http" type="http" location="US" preference="99" >$(cat httpd-address)/ostree/gnomerepo/nosuchfile</url>
        <url protocol="http" type="http" location="US" preference="98" >$(cat httpd-address)/ostree/gnomerepo/summary</url>
      </resources>
    </file>
EOF

cat > ${test_tmpdir}/metalink-data/metalink.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<metalink version="3.0" xmlns="http://www.metalinker.org/">
  <files>
    $(cat metalink-valid-summary.xml)
  </files>
</metalink>
EOF

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin metalink=$(cat metalink-httpd-address)/metalink.xml
${CMD_PREFIX} ostree --repo=repo pull origin:main
${CMD_PREFIX} ostree --repo=repo rev-parse origin:main
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull via metalink"

# Test fetching the summary through ostree_repo_remote_fetch_summary()
${CMD_PREFIX} ostree --repo=repo remote refs origin > origin_refs
assert_file_has_content origin_refs "main"
echo "ok remote refs via metalink"

cp metalink-data/metalink.xml{,.orig}
cp ostree-srv/gnomerepo/summary{,.orig}

test_metalink_pull_error() {
    msg=$1
    rm repo -rf
    mkdir repo
    ostree_repo_init repo
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin metalink=$(cat metalink-httpd-address)/metalink.xml
    if ${CMD_PREFIX} ostree --repo=repo pull origin:main 2>err.txt; then
	assert_not_reached "pull unexpectedly succeeded"
    fi
    assert_file_has_content err.txt "${msg}"
}

cd ${test_tmpdir}
sed -e 's,<hash type="sha512">.*</hash>,<hash type="sha512">bacon</hash>,' < metalink-data/metalink.xml.orig > metalink-data/metalink.xml
test_metalink_pull_error "Invalid hash digest for sha512"
echo "ok metalink err hash format"

cd ${test_tmpdir}
sed -e 's,<hash type="sha512">.*</hash>,<hash type="sha512">'$( (echo -n dummy; cat ${summary_path}) | sha512sum | cut -f 1 -d ' ')'</hash>,' < metalink-data/metalink.xml.orig > metalink-data/metalink.xml
test_metalink_pull_error "Expected checksum is .* but actual is"
echo "ok metalink err hash sha512"

cd ${test_tmpdir}
cp metalink-data/metalink.xml.orig metalink-data/metalink.xml
echo -n moo > ostree-srv/gnomerepo/summary
test_metalink_pull_error "Expected size is .* bytes but content is 3 bytes"
echo "ok metalink err size"
cp ostree-srv/gnomerepo/summary{.orig,}

cd ${test_tmpdir}
grep -v sha256 < metalink-data/metalink.xml.orig |grep -v sha512 > metalink-data/metalink.xml
test_metalink_pull_error "No.*verification.*with known.*hash"
echo "ok metalink err no verify"

cd ${test_tmpdir}
grep -v '<url protocol' < metalink-data/metalink.xml.orig > metalink-data/metalink.xml
test_metalink_pull_error "No.*url.*method.*elements"
echo "ok metalink err no url"

cd ${test_tmpdir}
echo bacon > metalink-data/metalink.xml
test_metalink_pull_error "Document must begin with an element"
echo "ok metalink err malformed"

cat > ${test_tmpdir}/metalink-data/metalink.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<metalink version="3.0" xmlns="http://www.metalinker.org/">
  <deeply>
    <nested>
      <unknown>
        <data>
          hello world
        </data>
      </unknown>
    </nested>
  </deeply>
  <files>
    $(cat metalink-valid-summary.xml)
  </files>
  <deeply>
    <nested>
      <unknown>
        <data>
          hello world
        </data>
      </unknown>
    </nested>
  </deeply>
</metalink>
EOF

cd ${test_tmpdir}
rm repo -rf
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin metalink=$(cat metalink-httpd-address)/metalink.xml
${CMD_PREFIX} ostree --repo=repo pull origin:main
${CMD_PREFIX} ostree --repo=repo rev-parse origin:main
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull via metalink with nested data"
