#!/bin/bash

# Using the host ostree, test HTTP pulls

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

test_tmpdir=$(prepare_tmpdir)
trap _tmpdir_cleanup EXIT

# Take the host's ostree, and make it archive
cd /var/srv
rm repo bare-repo -rf
mkdir repo
ostree --repo=repo init --mode=archive
echo -e '[archive]\nzlib-level=1\n' >> repo/config
host_nonremoteref=$(echo ${host_refspec} | sed 's,[^:]*:,,')
ostree --repo=repo pull-local /ostree/repo ${host_commit}
ostree --repo=repo refs ${host_commit} --create=${host_nonremoteref}

run_tmp_webserver $(pwd)/repo
# Now test pulling via HTTP (no deltas) to a new bare-user repo
ostree --repo=bare-repo init --mode=bare-user
ostree --repo=bare-repo remote add origin --set=gpg-verify=false $(cat ${test_tmpdir}/httpd-address)
ostree --repo=bare-repo pull --disable-static-deltas origin ${host_nonremoteref}
