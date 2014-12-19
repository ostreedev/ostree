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

set -e

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
ostree --repo=mirrorrepo init --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
$OSTREE show main >/dev/null
echo "ok pull mirror"

cd ${test_tmpdir}
ostree --repo=ostree-srv/gnomerepo commit -b main -s "Metadata string" --add-detached-metadata-string=SIGNATURE=HANCOCK --tree=ref=main
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
$OSTREE show --print-detached-metadata-key=SIGNATURE main > main-meta
assert_file_has_content main-meta "HANCOCK"
echo "ok pull detached metadata"

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
# Generate a delta from old to current, even though we aren't going to
# use it.
ostree --repo=ostree-srv/gnomerepo static-delta generate main

rm main-files -rf
ostree --repo=ostree-srv/gnomerepo checkout main main-files
cd main-files
echo "an added file for static deltas" > added-file
echo "modified file for static deltas" > baz/cow
rm baz/saucer
ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s 'static delta test'
cd ..
rm main-files -rf
# Generate delta that we'll use
ostree --repo=ostree-srv/gnomerepo static-delta generate main

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout origin:main checkout-origin-main
cd checkout-origin-main
assert_file_has_content firstfile '^first$'
assert_file_has_content baz/cow "modified file for static deltas"
assert_not_has_file baz/saucer

echo "ok static delta"

cd ${test_tmpdir}
rm main-files -rf
ostree --repo=ostree-srv/gnomerepo checkout main main-files
cd main-files
# Make a file larger than 16M for testing
dd if=/dev/zero of=test-bigfile count=1 seek=42678
echo "further modified file for static deltas" > baz/cow
ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s '2nd static delta test'
cd ..
rm main-files -rf
ostree --repo=ostree-srv/gnomerepo static-delta generate main

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
