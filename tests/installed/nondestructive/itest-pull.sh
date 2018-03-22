#!/bin/bash

# Using the host ostree, test HTTP pulls

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/../libinsttest.sh
date

prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT

# Take the host's ostree, and make it archive
mkdir repo
ostree --repo=repo init --mode=archive
echo -e '[archive]\nzlib-level=1\n' >> repo/config
host_nonremoteref=$(echo ${host_refspec} | sed 's,[^:]*:,,')
log_timestamps() {
    date
    "$@"
    date
}
log_timestamps ostree --repo=repo pull-local /ostree/repo ${host_commit}
ostree --repo=repo refs ${host_commit} --create=${host_nonremoteref}

run_tmp_webserver $(pwd)/repo
# Now test pulling via HTTP (no deltas) to a new bare-user repo
ostree --repo=bare-repo init --mode=bare-user
ostree --repo=bare-repo remote add origin --set=gpg-verify=false $(cat ${test_tmpdir}/httpd-address)
log_timestamps ostree --repo=bare-repo pull --disable-static-deltas origin ${host_nonremoteref}

rm bare-repo repo -rf

# Try copying the host's repo across a mountpoint for direct
# imports.
cd ${test_tmpdir}
mkdir tmpfs mnt
mount --bind tmpfs mnt
cd mnt
ostree --repo=repo init --mode=bare
log_timestamps ostree --repo=repo pull-local /ostree/repo ${host_commit}
log_timestamps ostree --repo=repo fsck
cd ..
umount mnt

kill -TERM $(cat ${test_tmpdir}/httpd-pid)
echo "ok"
date
