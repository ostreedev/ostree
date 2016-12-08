#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

function repo_init() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ${CMD_PREFIX} ostree --repo=repo init
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
}

function verify_initial_contents() {
    rm checkout-origin-main -rf
    $OSTREE checkout origin/main checkout-origin-main
    cd checkout-origin-main
    assert_file_has_content firstfile '^first$'
    assert_file_has_content baz/cow '^moo$'
}

echo "1..14"

# Try both syntaxes
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo pull origin:main
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull"

cd ${test_tmpdir}
verify_initial_contents
echo "ok pull contents"

cd ${test_tmpdir}
mkdir mirrorrepo
${CMD_PREFIX} ostree --repo=mirrorrepo init --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
$OSTREE show main >/dev/null
echo "ok pull mirror"

cd ${test_tmpdir}
rm checkout-origin-main -rf
$OSTREE --repo=ostree-srv/gnomerepo checkout main checkout-origin-main
echo moomoo > checkout-origin-main/baz/cow
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "" --tree=dir=checkout-origin-main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo fsck
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
echo "ok pull mirror (should not apply deltas)"

cd ${test_tmpdir}
rm mirrorrepo/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=mirrorrepo prune --refs-only
${CMD_PREFIX} ostree --repo=mirrorrepo pull origin main
rm checkout-origin-main -rf
$OSTREE --repo=ostree-srv/gnomerepo checkout main checkout-origin-main
echo yetmorecontent > checkout-origin-main/baz/cowtest
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "" --tree=dir=checkout-origin-main
rev=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo rev-parse main)
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
${CMD_PREFIX} ostree --repo=mirrorrepo pull --commit-metadata-only origin main
assert_has_file mirrorrepo/state/${rev}.commitpartial
echo "ok pull commit metadata only (should not apply deltas)"

cd ${test_tmpdir}
mkdir mirrorrepo-local
${CMD_PREFIX} ostree --repo=mirrorrepo-local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo-local remote add --set=gpg-verify=false origin file://$(pwd)/ostree-srv/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo-local pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo-local fsck
$OSTREE show main >/dev/null
echo "ok pull local mirror"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Metadata string" --add-detached-metadata-string=SIGNATURE=HANCOCK --tree=ref=main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
$OSTREE show --print-detached-metadata-key=SIGNATURE main > main-meta
assert_file_has_content main-meta "HANCOCK"
echo "ok pull detached metadata"

cd ${test_tmpdir}
mkdir parentpullrepo
${CMD_PREFIX} ostree --repo=parentpullrepo init --mode=archive-z2
${CMD_PREFIX} ostree --repo=parentpullrepo remote add --set=gpg-verify=false origin file://$(pwd)/ostree-srv/gnomerepo
parent_rev=$(ostree --repo=ostree-srv/gnomerepo rev-parse main^)
rev=$(ostree --repo=ostree-srv/gnomerepo rev-parse main)
${CMD_PREFIX} ostree --repo=parentpullrepo pull origin main@${parent_rev}
${CMD_PREFIX} ostree --repo=parentpullrepo rev-parse origin:main > main.txt
assert_file_has_content main.txt ${parent_rev}
${CMD_PREFIX} ostree --repo=parentpullrepo fsck
${CMD_PREFIX} ostree --repo=parentpullrepo pull origin main
${CMD_PREFIX} ostree --repo=parentpullrepo rev-parse origin:main > main.txt
assert_file_has_content main.txt ${rev}
echo "ok pull specific commit"

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
# Generate a delta from old to current, even though we aren't going to
# use it.
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main

rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main main-files
cd main-files
echo "an added file for static deltas" > added-file
echo "modified file for static deltas" > baz/cow
rm baz/saucer
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s 'static delta test'
cd ..
rm main-files -rf
# Generate delta that we'll use
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
prev_rev=$(ostree --repo=ostree-srv/gnomerepo rev-parse main^)
new_rev=$(ostree --repo=ostree-srv/gnomerepo rev-parse main)
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --dry-run --require-static-deltas origin main >dry-run-pull.txt
assert_file_has_content dry-run-pull.txt 'Delta update: 0/1 parts'
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_streq "${prev_rev}" "${rev}"
${CMD_PREFIX} ostree --repo=repo fsck

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin main
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_streq "${new_rev}" "${rev}"
${CMD_PREFIX} ostree --repo=repo fsck

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --disable-static-deltas origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout origin:main checkout-origin-main
cd checkout-origin-main
assert_file_has_content firstfile '^first$'
assert_file_has_content baz/cow "modified file for static deltas"
assert_not_has_file baz/saucer

echo "ok static delta"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate --swap-endianness main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas --dry-run origin main >byteswapped-dry-run-pull.txt
${CMD_PREFIX} ostree --repo=repo fsck

if ! diff -u dry-run-pull.txt byteswapped-dry-run-pull.txt; then
    assert_not_reached "byteswapped delta differs in size"
fi

echo "ok pull byteswapped delta"

cd ${test_tmpdir}
rm ostree-srv/gnomerepo/deltas -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
repo_init
if ${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin main 2>err.txt; then
    assert_not_reached "--require-static-deltas unexpectedly succeeded"
fi
assert_file_has_content err.txt "deltas required, but none found"
${CMD_PREFIX} ostree --repo=repo fsck

echo "ok delta required but don't exist"

cd ${test_tmpdir}
rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main main-files
cd main-files
echo "more added files for static deltas" > added-file2
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s 'inline static delta test'
cd ..
rm main-files -rf
# Generate new delta that we'll use
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate --inline main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout origin:main checkout-origin-main
cd checkout-origin-main
assert_file_has_content added-file2 "more added files for static deltas"

echo "ok inline static delta"

cd ${test_tmpdir}
rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main main-files
cd main-files
# Make a file larger than 16M for testing
dd if=/dev/zero of=test-bigfile count=1 seek=42678
echo "further modified file for static deltas" > baz/cow
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s '2nd static delta test'
cd ..
rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout origin:main checkout-origin-main
cd checkout-origin-main
assert_has_file test-bigfile
stat --format=%s test-bigfile > bigfile-size
assert_file_has_content bigfile-size 21851648
assert_file_has_content baz/cow "further modified file for static deltas"
assert_not_has_file baz/saucer

echo "ok static delta 2"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false --set=unconfigured-state="Access to ExampleOS requires ONE BILLION DOLLARS." origin-subscription file://$(pwd)/ostree-srv/gnomerepo
if ${CMD_PREFIX} ostree --repo=repo pull origin-subscription main 2>err.txt; then
    assert_not_reached "pull unexpectedly succeeded?"
fi
assert_file_has_content err.txt "ONE BILLION DOLLARS"

echo "ok unconfigured"
