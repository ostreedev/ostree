#!/usr/bin/env bash
#
# Generate multiple variants of static deltas in a repo, then use them
# to upgrade in a test repository, verifying that fsck works.
#
# This test is manual so it's easy to run against arbitrary content.

set -euo pipefail

repo=$(pwd)/${1}
shift
branch=${1}
shift

from=$(ostree --repo=${repo} rev-parse "${branch}"^)
to=$(ostree --repo=${repo} rev-parse ${branch})

tmpdir=$(mktemp -d /var/tmp/ostree-delta-check.XXXXXX)
cd ${tmpdir}
touch ${tmpdir}/.tmp
echo "Using tmpdir ${tmpdir}"

cleanup_tmpdir() {
    if test -f ${tmpdir}/.tmp; then
	rm -rf ${tmpdir}
    fi
}
    
if test -z "${PRESERVE_TMP:-}"; then
    trap cleanup_tmpdir EXIT
fi

fatal() {
    echo "$@"
    exit 1
}

assert_streq() {
    if ! test $1 = $2; then
	fatal "assertion failed: $1 = $2"
    fi
}

validate_delta_options() {
    mkdir testrepo
    ostree --repo=testrepo init --mode=bare-user
    ostree --repo=testrepo remote add --set=gpg-verify=false local file://${repo}
    ostree --repo=${repo} static-delta generate $@ --from=${from} --to=${to}
    ostree --repo=testrepo pull --require-static-deltas local ${branch}@${from}
    assert_streq $(ostree --repo=testrepo rev-parse ${branch}) ${from}
    ostree --repo=testrepo pull --require-static-deltas local ${branch}
    assert_streq $(ostree --repo=testrepo rev-parse ${branch}) ${to}
    ostree --repo=testrepo fsck
    rm testrepo -rf
}

set -x

validate_delta_options
validate_delta_options --inline
validate_delta_options --disable-bsdiff
