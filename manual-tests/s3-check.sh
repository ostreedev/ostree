#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.0+
#
# Check s3:// remotes against real AWS, which the automated tests can't.
#
#   s3-check.sh pull  <s3-url> <ref>
#   s3-check.sh bench <s3-url> <https-url> <ref>
#
# pull: pull <ref> into a new repo and fsck it. Run it once for each
# credential source to check: environment keys, an EC2, ECS or EKS role,
# and SSO, assume-role and credential_process profiles (add &profile=).
# With a role, also run it without ?region= to check region discovery.
#
# bench: pull <ref> from <https-url>, a public-read copy of the repo in the
# same region (a test-only bucket), and from <s3-url> at 8 requests and at
# the default. Passes if s3 at 8 is within 5% of https and reuses its
# connections, and the default is faster than 8. Run it from a machine in
# the bucket's region.
#
# Uses the ostree found in $OSTREE, else in $PATH. Uses whatever AWS
# credentials the environment provides and never prints them. Note that the
# curl debug output it captures includes the signed request headers
# (Authorization, and X-Amz-Security-Token for temporary credentials); that
# output stays in a private temporary directory which is removed on exit.

set -euo pipefail

ostree=${OSTREE:-ostree}
workdir=$(mktemp -d "${TMPDIR:-/tmp}/s3-check.XXXXXX")
trap 'rm -rf "${workdir}"' EXIT

usage () {
    sed -n '5,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

if ! "${ostree}" --version | grep -q -e '- aws$'; then
    echo "error: ${ostree} was built without the aws feature" >&2
    exit 1
fi

# one_pull <label> <url> <ref> [extra pull args...]
# Prints: label, seconds, objects, objects/sec, connections opened.
one_pull () {
    local label=$1 url=$2 ref=$3
    shift 3
    local repo="${workdir}/repo-${label}"
    rm -rf "${repo}"
    "${ostree}" --repo="${repo}" init --mode=bare-user >/dev/null
    "${ostree}" --repo="${repo}" remote add --set=gpg-verify=false origin "${url}"
    local start end
    start=$(date +%s.%N)
    OSTREE_DEBUG_HTTP=1 "${ostree}" --repo="${repo}" pull --disable-static-deltas "$@" origin "${ref}" \
        >/dev/null 2>"${workdir}/debug-${label}.txt"
    end=$(date +%s.%N)
    local objects connections seconds
    objects=$(find "${repo}/objects" -type f | wc -l)
    # curl >= 8.16 says "Established connection to", older curl "Connected to"
    connections=$(grep -c -E '^\* (Connected to|Established connection to) ' "${workdir}/debug-${label}.txt" || true)
    seconds=$(echo "${end} - ${start}" | bc -l)
    printf '%-14s %8.2fs %7d objects %8.1f obj/s %5d connections\n' \
        "${label}" "${seconds}" "${objects}" "$(echo "${objects} / ${seconds}" | bc -l)" "${connections}"
    # Machine-readable copy for the pass/fail checks in bench.
    echo "${seconds} ${connections}" > "${workdir}/result-${label}.txt"
}

cmd=${1:-}
case "${cmd}" in
    pull)
        test $# -eq 3 || usage
        one_pull s3 "$2" "$3"
        "${ostree}" --repo="${workdir}/repo-s3" fsck >/dev/null
        echo "PASS: pulled $3 and fsck is clean"
        ;;
    bench)
        test $# -eq 4 || usage
        s3_url=$2
        https_url=$3
        ref=$4
        one_pull https-8 "${https_url}" "${ref}" --max-outstanding-fetcher-requests=8
        one_pull s3-8 "${s3_url}" "${ref}" --max-outstanding-fetcher-requests=8
        one_pull s3-default "${s3_url}" "${ref}"
        read -r https_secs _ < "${workdir}/result-https-8.txt"
        read -r s3_8_secs s3_8_conns < "${workdir}/result-s3-8.txt"
        read -r s3_def_secs _ < "${workdir}/result-s3-default.txt"
        fail=0
        # A pull uses up to three fetcher instances, each with its own connections.
        if test "${s3_8_conns}" -gt 24; then
            echo "FAIL: s3 at concurrency 8 opened ${s3_8_conns} connections (limit 24): connections are not being reused"
            fail=1
        fi
        if test "$(echo "${s3_8_secs} > ${https_secs} * 1.05" | bc -l)" = 1; then
            echo "FAIL: s3 at concurrency 8 (${s3_8_secs}s) is more than 5% slower than https (${https_secs}s)"
            fail=1
        fi
        if test "$(echo "${s3_def_secs} >= ${s3_8_secs}" | bc -l)" = 1; then
            echo "FAIL: s3 at the default concurrency (${s3_def_secs}s) is not faster than at 8 (${s3_8_secs}s)"
            fail=1
        fi
        test ${fail} -eq 0 && echo "PASS: benchmark criteria met"
        exit ${fail}
        ;;
    *)
        usage
        ;;
esac
