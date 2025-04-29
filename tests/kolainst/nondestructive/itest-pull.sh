#!/bin/bash
# FIXME just for webserver
# kola: { "tags": "needs-internet" }
# Using the host ostree, test HTTP pulls

set -xeuo pipefail

# FIXME: https://github.com/ostreedev/ostree/pull/1548
exit 0

. ${KOLA_EXT_DATA}/libinsttest.sh
date

prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT

# Take the host's ostree, and make it archive
mkdir repo
ostree --repo=repo init --mode=archive
echo -e '[archive]\nzlib-level=1\n' >> repo/config
host_nonremoteref=testref
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

echo "ok pull"

# fsck marks commits partial
# https://github.com/ostreedev/ostree/pull/1533
for d in $(find bare-repo/objects/ -maxdepth 1 -type d); do
    (find ${d} -name '*.file' || true) | head -20 | xargs rm -f
done
date
if ostree --repo=bare-repo fsck |& tee fsck.txt; then
    fatal "fsck unexpectedly succeeded"
fi
date
assert_streq $(grep -cE -e 'Marking commit as partial' fsck.txt) "1"
log_timestamps ostree --repo=bare-repo pull origin ${host_nonremoteref}
# Don't need a full fsck here
ostree --repo=bare-repo ls origin:${host_nonremoteref} >/dev/null

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

# Also, we shouldn't copy xattrs on metadata objects
commit_path=objects/${host_commit:0:2}/${host_commit:2}.commit
ostree --repo=testarchive init --mode=archive
ostree --repo=testarchive pull-local --commit-metadata-only /ostree/repo ${host_commit}
setfattr -n user.ostreetesting -v hello testarchive/${commit_path}
ostree --repo=mnt/testarchive2 init --mode=archive
ostree --repo=mnt/testarchive2 pull-local --commit-metadata-only testarchive ${host_commit}
if getfattr -m user.ostreetesting mnt/testarchive2/${commit_path} 2>/dev/null; then
    fatal "copied metadata xattr"
fi
echo "ok no metadata xattr copy"

umount mnt

# Cleanup
kill -TERM $(cat ${test_tmpdir}/httpd-pid)
echo "ok"
date
