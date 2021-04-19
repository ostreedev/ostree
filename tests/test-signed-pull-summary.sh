#!/bin/bash
#
# Copyright (C) 2019 Collabora Ltd.
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

# Based on test-pull-summary-sigs.sh test.

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..14"

# Ensure repo caching is in use.
unset OSTREE_SKIP_CACHE

# This is explicitly opt in for testing
export OSTREE_DUMMY_SIGN_ENABLED=1

repo_reinit () {
    ARGS="$*"
    cd ${test_tmpdir}
    rm -rf repo
    mkdir repo
    ostree_repo_init repo --mode=archive
    ${OSTREE} --repo=repo remote add \
        --set=gpg-verify=false --set=gpg-verify-summary=false \
        --set=sign-verify=false --set=sign-verify-summary=true \
        ${ARGS} origin $(cat httpd-address)/ostree/gnomerepo
}

for engine in dummy ed25519
do
    case "${engine}" in
        dummy)
            # Tests with dummy engine
            SIGN_KEY="dummysign"
            PUBLIC_KEY="dummysign"
            ;;
        ed25519)
            if ! has_sign_ed25519; then
                echo "ok ${engine} pull mirror summary # SKIP due libsodium unavailability"
                echo "ok ${engine} pull with signed summary # SKIP due libsodium unavailability"
                echo "ok ${engine} prune summary cache # SKIP due libsodium unavailability"
                echo "ok ${engine} pull with signed summary and cachedir # SKIP due libsodium unavailability"
                echo "ok ${engine} pull with invalid ${engine} summary signature fails # SKIP due libsodium unavailability"
                echo "ok ${engine} pull delta with signed summary # SKIP due libsodium unavailability"

                continue
            fi
            gen_ed25519_keys
            SIGN_KEY="${ED25519SECRET}"
            PUBLIC_KEY="${ED25519PUBLIC}"
            ;;
        *)
            fatal "Unsupported engine ${engine}"
            ;;
    esac

    COMMIT_SIGN="--sign-type=${engine} --sign=${SIGN_KEY}"

    # clenup the testdir prior the next engine
    rm -rf ${test_tmpdir}/ostree-srv ${test_tmpdir}/gnomerepo ${test_tmpdir}/httpd ${test_tmpdir}/repo ${test_tmpdir}/cachedir\
        ${test_tmpdir}/main-copy ${test_tmpdir}/other-copy ${test_tmpdir}/yet-another-copy 

    setup_fake_remote_repo1 "archive" "${COMMIT_SIGN}"

    # Now, setup multiple branches
    mkdir ${test_tmpdir}/ostree-srv/other-files
    cd ${test_tmpdir}/ostree-srv/other-files
    echo 'hello world another object' > hello-world
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b other -s "A commit" -m "Another Commit body"

    mkdir ${test_tmpdir}/ostree-srv/yet-other-files
    cd ${test_tmpdir}/ostree-srv/yet-other-files
    echo 'hello world yet another object' > yet-another-hello-world
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b yet-another -s "A commit" -m "Another Commit body"

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u

    prev_dir=`pwd`
    cd ${test_tmpdir}
    ostree_repo_init repo --mode=archive
    ${CMD_PREFIX} ostree --repo=repo remote add \
        --set=gpg-verify=false --set=gpg-verify-summary=false \
        --set=sign-verify=false --set=sign-verify-summary=false \
        origin $(cat httpd-address)/ostree/gnomerepo
    ${CMD_PREFIX} ostree --repo=repo pull --mirror origin
    assert_has_file repo/summary
    ${CMD_PREFIX} ostree --repo=repo checkout -U main main-copy
    assert_file_has_content main-copy/baz/cow "moo"
    ${CMD_PREFIX} ostree --repo=repo checkout -U other other-copy
    assert_file_has_content other-copy/hello-world "hello world another object"
    ${CMD_PREFIX} ostree --repo=repo checkout -U yet-another yet-another-copy
    assert_file_has_content yet-another-copy/yet-another-hello-world "hello world yet another object"
    ${CMD_PREFIX} ostree --repo=repo fsck
    echo "ok ${engine} pull mirror summary"


    cd $prev_dir

    ${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

    cd ${test_tmpdir}
    repo_reinit --set=verification-${engine}-key=${PUBLIC_KEY}
    ${OSTREE} --repo=repo pull origin main
    assert_has_file repo/tmp/cache/summaries/origin
    assert_has_file repo/tmp/cache/summaries/origin.sig

    rm repo/tmp/cache/summaries/origin
    ${OSTREE} --repo=repo pull origin main
    assert_has_file repo/tmp/cache/summaries/origin

    echo "ok ${engine} pull with signed summary"

    touch repo/tmp/cache/summaries/foo
    touch repo/tmp/cache/summaries/foo.sig
    ${OSTREE} --repo=repo prune
    assert_not_has_file repo/tmp/cache/summaries/foo
    assert_not_has_file repo/tmp/cache/summaries/foo.sig
    assert_has_file repo/tmp/cache/summaries/origin
    assert_has_file repo/tmp/cache/summaries/origin.sig
    echo "ok ${engine} prune summary cache"

    cd ${test_tmpdir}
    repo_reinit --set=verification-${engine}-key=${PUBLIC_KEY}
    mkdir cachedir
    ${OSTREE} --repo=repo pull --cache-dir=cachedir origin main
    assert_not_has_file repo/tmp/cache/summaries/origin
    assert_not_has_file repo/tmp/cache/summaries/origin.sig
    assert_has_file cachedir/summaries/origin
    assert_has_file cachedir/summaries/origin.sig

    rm cachedir/summaries/origin
    ${OSTREE} --repo=repo pull --cache-dir=cachedir origin main
    assert_not_has_file repo/tmp/cache/summaries/origin
    assert_has_file cachedir/summaries/origin

    echo "ok ${engine} pull with signed summary and cachedir"

    cd ${test_tmpdir}
    repo_reinit --set=verification-${engine}-key=${PUBLIC_KEY}
    mv ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{,.good}
    echo invalid > ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig
    if ${OSTREE} --repo=repo pull origin main 2>err.txt; then
        assert_not_reached "Successful pull with invalid ${engine} signature"
    fi
    assert_file_has_content err.txt "No signatures found"
    mv ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.good,}
    echo "ok ${engine} pull with invalid ${engine} summary signature fails"

    # Generate a delta
    ${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo static-delta generate --empty main
    ${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}

    cd ${test_tmpdir}
    repo_reinit --set=verification-${engine}-key=${PUBLIC_KEY}
    ${OSTREE} --repo=repo pull origin main
    echo "ok ${engine} pull delta with signed summary"

done

if ! has_sign_ed25519; then
    echo "ok ${engine} pull with signed summary remote old summary # SKIP due libsodium unavailability"
    echo "ok ${engine} pull with signed summary broken cache # SKIP due libsodium unavailability"
    exit 0
fi

gen_ed25519_keys
SIGN_KEY="${ED25519SECRET}"
PUBLIC_KEY="${ED25519PUBLIC}"
COMMIT_SIGN="--sign-type=${engine} --sign=${SIGN_KEY}"


# Verify 'ostree remote summary' output.
${OSTREE} --repo=repo remote summary origin > summary.txt
assert_file_has_content summary.txt "* main"
assert_file_has_content summary.txt "* other"
assert_file_has_content summary.txt "* yet-another"
grep static-deltas summary.txt > static-deltas.txt
assert_file_has_content static-deltas.txt \
    $(${OSTREE} --repo=repo rev-parse origin:main)

## Tests for handling of cached summaries while racing with remote summary updates

# Make 2 different but valid summary/signature pairs to test races with
${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{,.1}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{,.1}
mkdir ${test_tmpdir}/ostree-srv/even-another-files
cd ${test_tmpdir}/ostree-srv/even-another-files
echo 'hello world even another object' > even-another-hello-world
${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_SIGN} -b even-another -s "A commit" -m "Another Commit body"
${OSTREE} --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u ${COMMIT_SIGN}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{,.2}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{,.2}
cd ${test_tmpdir}

# Reset to the old valid summary and pull to cache it
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{.1,}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.1,}
repo_reinit --set=verification-ed25519-key=${PUBLIC_KEY}
${OSTREE} --repo=repo pull origin main
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.1 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.1 >&2

# Simulate a pull race where the client gets the old summary and the new
# summary signature since it was generated on the server between the
# requests
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.2,}
if ${OSTREE} --repo=repo pull origin main 2>err.txt; then
    assert_not_reached "Successful pull with old summary"
fi
assert_file_has_content err.txt "ed25519: Signature couldn't be verified with: key"
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.1 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.1 >&2

# Publish correct summary and check that subsequent pull succeeds
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{.2,}
${OSTREE} --repo=repo pull origin main
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.2 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.2 >&2

echo "ok ${engine} pull with signed summary remote old summary"


# Reset to the old valid summary and pull to cache it
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{.1,}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.1,}
repo_reinit --set=verification-ed25519-key=${PUBLIC_KEY}
${OSTREE} --repo=repo pull origin main
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.1 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.1 >&2

# Simulate a broken summary cache to see if it can be recovered from.
# Prior to commit c4c2b5eb the client would save the summary to the
# cache before validating the signature. That would mean the cache would
# have mismatched summary and signature and ostree would remain
# deadlocked there until the remote published a new signature.
#
# First pull with OSTREE_REPO_TEST_ERROR=invalid-cache to see the
# invalid cache is detected. Then pull again to check if it can be
# recovered from.
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.2 repo/tmp/cache/summaries/origin
if OSTREE_REPO_TEST_ERROR=invalid-cache ${OSTREE} --repo=repo pull origin main 2>err.txt; then
    assert_not_reached "Should have hit OSTREE_REPO_TEST_ERROR_INVALID_CACHE"
fi
assert_file_has_content err.txt "OSTREE_REPO_TEST_ERROR_INVALID_CACHE"
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.2 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.1 >&2
${OSTREE} --repo=repo pull origin main
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.1 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.1 >&2

# Publish new signature and check that subsequent pull succeeds
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary{.2,}
cp ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig{.2,}
${OSTREE} --repo=repo pull origin main
assert_has_file repo/tmp/cache/summaries/origin
assert_has_file repo/tmp/cache/summaries/origin.sig
cmp repo/tmp/cache/summaries/origin ${test_tmpdir}/ostree-srv/gnomerepo/summary.2 >&2
cmp repo/tmp/cache/summaries/origin.sig ${test_tmpdir}/ostree-srv/gnomerepo/summary.sig.2 >&2

echo "ok ${engine} pull with signed summary broken cache"

