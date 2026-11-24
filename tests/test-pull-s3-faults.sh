#!/bin/bash
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

# Repo safety under failure for s3:// pulls, in particular for large objects
# fetched as several ranged requests. Whatever the server does, a failed pull
# must leave no partial or mixed object in the repo, must leave the commit
# marked partial, and must be recoverable by simply pulling again.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature aws
if ! command -v python3 >/dev/null; then
    skip "python3 is required"
fi

echo '1..17'

cd ${test_tmpdir}
. $(dirname $0)/libtest-s3.sh
export AWS_ACCESS_KEY_ID=faultkey AWS_SECRET_ACCESS_KEY=faultsecret

# --- Server-side repo: a few small files and one incompressible 12 MiB file,
# --- which is fetched as three 4 MiB ranges.
mkdir -p files/sub
for i in $(seq 1 20); do
    echo "content ${i}" > files/sub/file-${i}
done
head -c 12582912 /dev/urandom > files/big.bin
mkdir -p root/bkt
ostree_repo_init root/bkt/repo --mode=archive
${CMD_PREFIX} ostree --repo=root/bkt/repo commit -b main -s c --tree=dir=files >/dev/null
${CMD_PREFIX} ostree --repo=root/bkt/repo summary -u
rev=$(${CMD_PREFIX} ostree --repo=root/bkt/repo rev-parse main)

port=$(free_port)
echo ok > mode
: > requests.log
python3 $(dirname $0)/s3-fault-server.py ${test_tmpdir}/root ${test_tmpdir}/mode \
        ${test_tmpdir}/requests.log ${port} > server.log 2>&1 &
server_pid=$!
kill_server () {
    kill ${server_pid} 2>/dev/null || true
}
libtest_exit_cmds+=(kill_server)
wait_for_url http://127.0.0.1:${port}/bkt/repo/config "fault server"

s3url="s3://bkt/repo?endpoint=http://127.0.0.1:${port}&region=us-east-1"

set_mode () {
    echo $1 > mode
    : > requests.log
}

new_client_repo () {
    rm -rf $1
    ostree_repo_init $1 "${@:2}"
    ${CMD_PREFIX} ostree --repo=$1 remote add --set=gpg-verify=false origin "${s3url}"
}

# Number of objects in a repo that are large enough to be (part of) big.bin.
count_big_objects () {
    find $1/objects -type f -size +2M | wc -l
}

# Whether the follow-up ranges of the big object were requested.
ranges_requested () {
    grep -q '^4194304-8388607 ' requests.log && grep -q '^8388608-12' requests.log
}

assert_pull_ok () {
    ${CMD_PREFIX} ostree --repo=$1 pull --disable-static-deltas "${@:2}" origin main
    assert_streq "$(${CMD_PREFIX} ostree --repo=$1 rev-parse main)" "${rev}"
    assert_not_has_file $1/state/${rev}.commitpartial
    ${CMD_PREFIX} ostree --repo=$1 fsck >/dev/null
    test "$(count_big_objects $1)" -eq 1 || fatal "expected the big object to be present in $1"
}

# A pull under a persistent fault must fail, leave no trace of the big object,
# leave the commit marked partial, and leave a repo that fsck accepts.
assert_pull_fails_cleanly () {
    if ${CMD_PREFIX} ostree --repo=$1 pull --disable-static-deltas --network-retries=0 \
                     "${@:2}" origin main 2>err.txt; then
        fatal "pull succeeded in mode $(cat mode)"
    fi
    test "$(count_big_objects $1)" -eq 0 \
        || fatal "a partial or corrupt big object was left in $1 (mode $(cat mode))"
    assert_has_file $1/state/${rev}.commitpartial
    ${CMD_PREFIX} ostree --repo=$1 fsck >/dev/null
}

# 1: no faults; the big object arrives in ranges and is intact
new_client_repo repo1
set_mode ok
assert_pull_ok repo1
ranges_requested || fatal "big object was not fetched in ranges"
${CMD_PREFIX} ostree --repo=repo1 checkout main checkout1
cmp files/big.bin checkout1/big.bin
echo "ok ranged pull"

# 2: a follow-up range fails outright; then the server recovers
new_client_repo repo2
set_mode child500
assert_pull_fails_cleanly repo2
set_mode ok
assert_pull_ok repo2
echo "ok failed range, then recovery"

# 3: a follow-up range is cut off halfway
new_client_repo repo3
set_mode childshort
assert_pull_fails_cleanly repo3
set_mode ok
assert_pull_ok repo3
echo "ok truncated range, then recovery"

# 4: the server ignores Range on follow-ups and sends the whole object
new_client_repo repo4
set_mode child200
assert_pull_fails_cleanly repo4
echo "ok range ignored by server"

# 5: a follow-up range carries the wrong bytes; ostree's checksum catches it
new_client_repo repo5
set_mode childgarbage
assert_pull_fails_cleanly repo5
echo "ok corrupt range rejected by checksum"

# 6: the object changes between the first part and the follow-ups
new_client_repo repo6
set_mode etagchange
assert_pull_fails_cleanly repo6
# every follow-up range must have been conditional on the first part's ETag
if grep '^4194304-' requests.log | grep -q -v ' "v1-'; then
    cat requests.log
    fatal "a follow-up range was sent without If-Match"
fi
echo "ok changed object not stitched together"

# 7: without an ETag there is nothing to tie the parts together, so the
#    object must be fetched in one piece
new_client_repo repo7
set_mode noetag
assert_pull_ok repo7
if grep -q '^4194304-' requests.log; then
    cat requests.log
    fatal "follow-up ranges were sent although the server gave no ETag"
fi
echo "ok no ETag, no ranges"

# 8: a weak ETag cannot be used with If-Match either
new_client_repo repo8
set_mode weaketag
assert_pull_ok repo8
if grep -q '^4194304-' requests.log; then
    cat requests.log
    fatal "follow-up ranges were sent although the server gave only a weak ETag"
fi
echo "ok weak ETag, no ranges"

# 9: --http-trusted mirroring links the fetched file into the repo without
#    checksumming it, so the assembled file itself has to be right; fsck
#    then verifies it independently
new_client_repo mirror9 --mode=archive
set_mode ok
${CMD_PREFIX} ostree --repo=mirror9 pull --mirror --http-trusted --disable-static-deltas origin main
ranges_requested || fatal "big object was not fetched in ranges"
${CMD_PREFIX} ostree --repo=mirror9 fsck >/dev/null
test "$(count_big_objects mirror9)" -eq 1 || fatal "big object missing from mirror"
echo "ok trusted mirror with ranges"

# 10: the same unverified path under a fault must not link a partial file
new_client_repo mirror10 --mode=archive
set_mode childshort
if ${CMD_PREFIX} ostree --repo=mirror10 pull --mirror --http-trusted --disable-static-deltas \
                 --network-retries=0 origin main 2>err.txt; then
    fatal "trusted mirror pull succeeded with a truncated range"
fi
test "$(count_big_objects mirror10)" -eq 0 || fatal "a partial object was linked into the mirror"
set_mode ok
${CMD_PREFIX} ostree --repo=mirror10 pull --mirror --http-trusted --disable-static-deltas origin main
${CMD_PREFIX} ostree --repo=mirror10 fsck >/dev/null
test "$(count_big_objects mirror10)" -eq 1 || fatal "big object missing from mirror after recovery"
echo "ok trusted mirror never links a partial file"

# For the unverified --http-trusted path: a pull under a malformed response
# must fail and must not link the big object into the mirror.
assert_trusted_mirror_rejects () {
    new_client_repo $1 --mode=archive
    set_mode $2
    if ${CMD_PREFIX} ostree --repo=$1 pull --mirror --http-trusted --disable-static-deltas \
                     --network-retries=0 origin main 2>err.txt; then
        fatal "trusted mirror pull succeeded in mode $2"
    fi
    test "$(count_big_objects $1)" -eq 0 || fatal "a corrupt object was linked into $1 (mode $2)"
}

# 11: a first range answered 206 without saying which bytes it holds
assert_trusted_mirror_rejects mirror11 firstnorange
echo "ok first range without Content-Range rejected"

# 12: a first range whose body is shorter than its Content-Range says
assert_trusted_mirror_rejects mirror12 firstshort
echo "ok short first range rejected"

# 13: a follow-up range that is a different range from the one requested
assert_trusted_mirror_rejects mirror13 childshifted
echo "ok mislabelled follow-up range rejected"

# 14: a request limit of 0 means "use the default", not "never start"
new_client_repo repo14
set_mode ok
timeout 120 ${CMD_PREFIX} ostree --repo=repo14 pull --disable-static-deltas \
    --max-outstanding-fetcher-requests=0 origin main \
    || fatal "pull with --max-outstanding-fetcher-requests=0 failed or hung (status $?)"
assert_streq "$(${CMD_PREFIX} ostree --repo=repo14 rev-parse main)" "${rev}"
echo "ok request limit of 0"

# 15: an empty 206 to the first range, with no Content-Range
assert_trusted_mirror_rejects mirror15 firstempty
echo "ok empty first range rejected"

# 16: a first range that drops while its other ranges are still downloading.
# Run several such pulls in one process, as a long-lived library user would:
# no range may outlive its pull (to free things underneath it later, or to
# keep its temporary file open). Under valgrind when available, since a
# use-after-free does not always crash.
set_mode firstdrop
repeat_cmd=(${test_builddir}/s3-pull-repeat ${test_tmpdir} "${s3url}" 3)
if command -v valgrind >/dev/null; then
    # The helper is a libtool wrapper script; run valgrind on the real binary.
    repeat_cmd=(${top_builddir}/libtool --mode=execute valgrind -q --error-exitcode=99
                --suppressions=${test_srcdir}/glib.supp --suppressions=${test_srcdir}/ostree.supp
                "${repeat_cmd[@]}")
fi
rc=0
G_SLICE=always-malloc MALLOC_PERTURB_=165 "${repeat_cmd[@]}" > repeat.log 2>&1 || rc=$?
if test ${rc} -ne 0; then
    cat repeat.log
    fatal "in-process pulls failed with status ${rc}"
fi
assert_file_has_content repeat.log "no temporary files left open"
echo "ok no range outlives a failed pull"

# 17: pulls cancelled while all nine follow-up ranges of a 40 MiB object finish
# together. A range that already completed must not be failed again when its
# siblings are cancelled (GLib logs a critical; the helper makes it fatal).
mkdir -p files17
head -c 41943040 /dev/urandom > files17/big.bin
ostree_repo_init root/bkt/repo17 --mode=archive
${CMD_PREFIX} ostree --repo=root/bkt/repo17 commit -b main -s c --tree=dir=files17 >/dev/null
${CMD_PREFIX} ostree --repo=root/bkt/repo17 summary -u
set_mode childslow
mkdir cancel17
rc=0
${test_builddir}/s3-pull-repeat ${test_tmpdir}/cancel17 \
    "s3://bkt/repo17?endpoint=http://127.0.0.1:${port}&region=us-east-1" 5 500 \
    > cancel.log 2>&1 || rc=$?
if test ${rc} -ne 0; then
    cat cancel.log
    fatal "cancelled in-process pulls failed with status ${rc}"
fi
assert_file_has_content cancel.log "no temporary files left open"
echo "ok cancelling a pull with ranges in flight"
