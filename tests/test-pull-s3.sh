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

# End-to-end pulls from s3:// remotes against a local MinIO server.
# MinIO is used because it verifies SigV4 signatures and issues temporary
# (session-token) credentials through its STS endpoint.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature aws
for tool in minio mc curl python3; do
    if ! command -v ${tool} >/dev/null; then
        skip "${tool} is required"
    fi
done
if ! curl --help all | grep -q -e '--aws-sigv4'; then
    skip "curl with --aws-sigv4 is required to request temporary credentials"
fi

echo '1..11'

cd ${test_tmpdir}
. $(dirname $0)/libtest-s3.sh

# --- Server-side repo: ~200 objects, with a from-scratch static delta whose
# --- path contains '+', which S3 only serves when it is percent-encoded.
mkdir -p files/sub
for i in $(seq 1 200); do
    echo "content ${i}" > files/sub/file-${i}
done
# One incompressible 20 MiB file, large enough to be fetched in several ranges.
head -c 20971520 /dev/urandom > files/big.bin
ostree_repo_init srv --mode=archive
found_plus=false
for attempt in $(seq 1 60); do
    echo "attempt ${attempt}" > files/marker
    ${CMD_PREFIX} ostree --repo=srv commit -b main -s "commit ${attempt}" --tree=dir=files >/dev/null
    rm -rf srv/deltas
    ${CMD_PREFIX} ostree --repo=srv static-delta generate --empty main >/dev/null
    if find srv/deltas -path '*+*' | grep -q .; then
        found_plus=true
        break
    fi
done
${found_plus} || fatal "could not produce a static delta path containing '+'"
${CMD_PREFIX} ostree --repo=srv summary -u
srv_rev=$(${CMD_PREFIX} ostree --repo=srv rev-parse main)

# --- MinIO
root_user=ostreetestroot
root_secret=ostreetestrootsecret
port=$(free_port)
endpoint=http://127.0.0.1:${port}
mkdir minio-data
MINIO_ROOT_USER=${root_user} MINIO_ROOT_PASSWORD=${root_secret} \
    minio server --quiet --address 127.0.0.1:${port} ${test_tmpdir}/minio-data > minio.log 2>&1 &
minio_pid=$!
kill_minio () {
    kill ${minio_pid} 2>/dev/null || true
}
libtest_exit_cmds+=(kill_minio)
wait_for_url ${endpoint}/minio/health/live "MinIO (see minio.log)"

export MC_CONFIG_DIR=${test_tmpdir}/mc-config
mc --quiet alias set t ${endpoint} ${root_user} ${root_secret} >/dev/null
mc --quiet mb t/ostree-test >/dev/null
mc --quiet cp --recursive srv/ t/ostree-test/repo/ >/dev/null

s3url="s3://ostree-test/repo?endpoint=${endpoint}&region=us-east-1"

# Run ostree with the given access key and secret, and optionally a session token.
ostree_as () {
    local key=$1 secret=$2 token=$3
    shift 3
    if test -n "${token}"; then
        AWS_ACCESS_KEY_ID=${key} AWS_SECRET_ACCESS_KEY=${secret} AWS_SESSION_TOKEN=${token} \
            ${CMD_PREFIX} ostree "$@"
    else
        AWS_ACCESS_KEY_ID=${key} AWS_SECRET_ACCESS_KEY=${secret} ${CMD_PREFIX} ostree "$@"
    fi
}

new_client_repo () {
    rm -rf $1
    ostree_repo_init $1
    ${CMD_PREFIX} ostree --repo=$1 remote add --set=gpg-verify=false origin "$2"
}

# 1: static credentials from the environment
new_client_repo repo1 "${s3url}"
ostree_as ${root_user} ${root_secret} "" --repo=repo1 pull --disable-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo1 rev-parse main)" "${srv_rev}"
${CMD_PREFIX} ostree --repo=repo1 fsck >/dev/null
echo "ok pull with static credentials"

# 2: temporary credentials carrying a session token
mc --quiet admin user add t ostreetempuser ostreetempsecret >/dev/null
mc --quiet admin policy attach t readwrite --user ostreetempuser >/dev/null
sts_xml=$(curl -sS --aws-sigv4 "aws:amz:us-east-1:sts" --user ostreetempuser:ostreetempsecret \
               -d "Action=AssumeRole&Version=2011-06-15&DurationSeconds=900" ${endpoint}/)
xml_field () {
    echo "${sts_xml}" | sed -n "s:.*<$1>\([^<]*\)</$1>.*:\1:p"
}
temp_key=$(xml_field AccessKeyId)
temp_secret=$(xml_field SecretAccessKey)
temp_token=$(xml_field SessionToken)
test -n "${temp_key}" -a -n "${temp_secret}" -a -n "${temp_token}" \
    || fatal "MinIO STS did not return temporary credentials: ${sts_xml}"
new_client_repo repo2 "${s3url}"
ostree_as ${temp_key} ${temp_secret} ${temp_token} --repo=repo2 pull --disable-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo2 rev-parse main)" "${srv_rev}"
# The same key and secret without the token must be refused, proving the token is sent and signed.
new_client_repo repo2b "${s3url}"
if ostree_as ${temp_key} ${temp_secret} "" --repo=repo2b pull origin main 2>err.txt; then
    fatal "pulled with temporary credentials but no session token"
fi
echo "ok pull with temporary credentials"

# 3: static deltas, whose path contains '+'
new_client_repo repo3 "${s3url}"
ostree_as ${root_user} ${root_secret} "" --repo=repo3 pull --require-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo3 rev-parse main)" "${srv_rev}"
echo "ok pull with static deltas"

# 4: a wrong secret is a loud, identifiable failure
new_client_repo repo4 "${s3url}"
if ostree_as ${root_user} wrong-secret "" --repo=repo4 pull origin main 2>err.txt; then
    fatal "pulled with a wrong secret key"
fi
assert_file_has_content err.txt "SignatureDoesNotMatch"
echo "ok wrong credentials rejected"

# 5: a missing ref is "not found", not an authorization error
new_client_repo repo5 "${s3url}"
if ostree_as ${root_user} ${root_secret} "" --repo=repo5 pull origin nosuchref 2>err.txt; then
    fatal "pulled a ref that does not exist"
fi
assert_file_has_content err.txt "nosuchref"
assert_not_file_has_content err.txt "AccessDenied"
assert_not_file_has_content err.txt "SignatureDoesNotMatch"
echo "ok missing ref"

# 6: an unknown URL parameter is rejected instead of silently ignored
new_client_repo repo6 "s3://ostree-test/repo?endpoint=${endpoint}&regoin=us-east-1"
if ostree_as ${root_user} ${root_secret} "" --repo=repo6 pull origin main 2>err.txt; then
    fatal "pulled with an unknown URL parameter"
fi
assert_file_has_content err.txt "Unknown parameter 'regoin'"
echo "ok unknown parameter rejected"

# 7: no credentials anywhere fails cleanly, naming what was tried
new_client_repo repo7 "${s3url}"
if ${CMD_PREFIX} ostree --repo=repo7 pull origin main 2>err.txt; then
    fatal "pulled without any credentials"
fi
assert_file_has_content err.txt "No AWS credentials available"
echo "ok no credentials"

# 8: an ?endpoint= remote with no region anywhere must not wait on the EC2
# metadata service before falling back to a default signing region
new_client_repo repo8 "s3://ostree-test/repo?endpoint=${endpoint}"
start=$(date +%s)
(unset AWS_EC2_METADATA_DISABLED
 ostree_as ${root_user} ${root_secret} "" --repo=repo8 pull --disable-static-deltas origin main)
elapsed=$(( $(date +%s) - start ))
assert_streq "$(${CMD_PREFIX} ostree --repo=repo8 rev-parse main)" "${srv_rev}"
test "${elapsed}" -le 5 || fatal "endpoint pull without a region took ${elapsed}s"
echo "ok endpoint without region"

# 9: a 404 names what S3 says is missing, and is still "not found"
new_client_repo repo9 "s3://no-such-bucket/repo?endpoint=${endpoint}&region=us-east-1"
if ostree_as ${root_user} ${root_secret} "" --repo=repo9 pull origin main 2>err.txt; then
    fatal "pulled from a bucket that does not exist"
fi
assert_file_has_content err.txt "HTTP 404 NoSuchBucket"
echo "ok missing bucket"

# 10: --max-outstanding-fetcher-requests controls how many connections are opened
count_connections () {
    new_client_repo repo10 "${s3url}"
    OSTREE_DEBUG_HTTP=1 ostree_as ${root_user} ${root_secret} "" --repo=repo10 pull \
        --disable-static-deltas --max-outstanding-fetcher-requests=$1 origin main 2>debug.txt >/dev/null
    # curl >= 8.16 says "Established connection to", older curl "Connected to"
    grep -c -E '^\* (Connected to|Established connection to) ' debug.txt || true
}
serial=$(count_connections 1)
parallel=$(count_connections 16)
# One connection per fetcher instance (a pull creates at most three), plus
# four for the extra ranges of the 20 MiB object, which are fetched in parallel
# whatever the request limit is.
test "${serial}" -ge 1 -a "${serial}" -le 7 \
    || fatal "expected 1-7 connections at concurrency 1, got ${serial}"
test "${parallel}" -gt "${serial}" \
    || fatal "expected more connections at concurrency 16 (${parallel}) than at 1 (${serial})"
test "${parallel}" -le 48 \
    || fatal "expected at most 16 connections per fetcher at concurrency 16, got ${parallel}"
echo "ok connection count follows the concurrency limit"

# 11: a large object is fetched as parallel ranged requests, and the result
# still passes ostree's own checksum verification
new_client_repo repo11 "${s3url}"
OSTREE_DEBUG_HTTP=1 ostree_as ${root_user} ${root_secret} "" --repo=repo11 pull \
    --disable-static-deltas origin main 2>debug.txt >/dev/null
assert_file_has_content debug.txt 'Range: bytes=0-4194303'
assert_file_has_content debug.txt 'Range: bytes=4194304-8388607'
assert_file_has_content debug.txt 'Range: bytes=16777216-'
${CMD_PREFIX} ostree --repo=repo11 fsck >/dev/null
${CMD_PREFIX} ostree --repo=repo11 checkout main checkout11
cmp files/big.bin checkout11/big.bin
echo "ok large object fetched in ranges"
