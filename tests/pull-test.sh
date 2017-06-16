# This file is to be sourced, not executed

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
    ostree_repo_init repo
    ${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo "$@"
}

function verify_initial_contents() {
    rm checkout-origin-main -rf
    $OSTREE checkout origin/main checkout-origin-main
    cd checkout-origin-main
    assert_file_has_content firstfile '^first$'
    assert_file_has_content baz/cow '^moo$'
}

echo "1..25"

# Try both syntaxes
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo pull origin:main
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull"

cd ${test_tmpdir}
verify_initial_contents
echo "ok pull contents"

cd ${test_tmpdir}
mkdir mirrorrepo
ostree_repo_init mirrorrepo --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
$OSTREE show main >/dev/null
echo "ok pull mirror"

mkdir otherbranch
echo someothercontent > otherbranch/someothercontent
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b otherbranch --tree=dir=otherbranch
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
rm mirrorrepo -rf
# All refs
ostree_repo_init mirrorrepo --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
for ref in main otherbranch; do
    ${CMD_PREFIX} ostree --repo=mirrorrepo rev-parse $ref
done
echo "ok pull mirror (all refs)"

rm mirrorrepo -rf
ostree_repo_init mirrorrepo --mode=archive-z2
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
# Generate a summary in the mirror
${CMD_PREFIX} ostree --repo=mirrorrepo summary -u
summarysig=$(sha256sum < mirrorrepo/summary | cut -f 1 -d ' ')
# Mirror subset of refs: https://github.com/ostreedev/ostree/issues/846
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
newsummarysig=$(sha256sum < mirrorrepo/summary | cut -f 1 -d ' ')
assert_streq ${summarysig} ${newsummarysig}
echo "ok pull mirror (ref subset with summary)"

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
if ${CMD_PREFIX} ostree --repo=mirrorrepo \
     pull origin main --require-static-deltas 2>err.txt; then
  assert_not_reached "--require-static-deltas unexpectedly succeeded"
fi
assert_file_has_content err.txt "Can't use static deltas in an archive repo"
${CMD_PREFIX} ostree --repo=mirrorrepo pull origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
echo "ok pull (refuses deltas)"

cd ${test_tmpdir}
rm mirrorrepo/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=mirrorrepo prune --refs-only
${CMD_PREFIX} ostree --repo=mirrorrepo pull --bareuseronly-files origin main
echo "ok pull (bareuseronly, safe)"

rm checkout-origin-main -rf
$OSTREE --repo=ostree-srv/gnomerepo checkout main checkout-origin-main
cat > statoverride.txt <<EOF
2048 /some-setuid
EOF
echo asetuid > checkout-origin-main/some-setuid
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b content-with-suid --statoverride=statoverride.txt --tree=dir=checkout-origin-main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
# Verify we reject it both when unpacking and when mirroring
for flag in "" "--mirror"; do
    if ${CMD_PREFIX} ostree --repo=mirrorrepo pull ${flag} --bareuseronly-files origin content-with-suid 2>err.txt; then
        assert_not_reached "pulled unsafe bareuseronly"
    fi
    assert_file_has_content err.txt 'object.*\.file: invalid mode.*with bits 040.*'
done
echo "ok pull (bareuseronly, unsafe)"

cd ${test_tmpdir}
rm mirrorrepo/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=mirrorrepo prune --refs-only
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror --bareuseronly-files origin main
echo "ok pull (bareuseronly mirror)"

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
ostree_repo_init mirrorrepo-local --mode=archive-z2
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
ostree_repo_init parentpullrepo --mode=archive-z2
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
repo_init --no-gpg-verify
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

# Explicitly test delta fetches via ref name as well as commit hash
for delta_target in main ${new_rev}; do
cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --dry-run --require-static-deltas origin ${delta_target} >dry-run-pull.txt
# Compression can vary, so we support 400-699
assert_file_has_content dry-run-pull.txt 'Delta update: 0/1 parts, 0 bytes/[456][0-9][0-9] bytes, 455 bytes total uncompressed'
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_streq "${prev_rev}" "${rev}"
${CMD_PREFIX} ostree --repo=repo fsck
done

# Explicitly test delta fetches via ref name as well as commit hash
for delta_target in main ${new_rev}; do
cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin ${delta_target}
if test ${delta_target} = main; then
    rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
    assert_streq "${new_rev}" "${rev}"
else
    ${CMD_PREFIX} ostree --repo=repo rev-parse ${delta_target}
fi
${CMD_PREFIX} ostree --repo=repo fsck
done

cd ${test_tmpdir}
repo_init --no-gpg-verify
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

repo_init --no-gpg-verify
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
repo_init --no-gpg-verify
if ${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin main 2>err.txt; then
    assert_not_reached "--require-static-deltas unexpectedly succeeded"
fi
assert_file_has_content err.txt "deltas required, but none found"
${CMD_PREFIX} ostree --repo=repo fsck

# Now test with a partial commit
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull --commit-metadata-only origin main@${prev_rev}
if ${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin main 2>err.txt; then
    assert_not_reached "--require-static-deltas unexpectedly succeeded"
fi
assert_file_has_content err.txt "deltas required, but none found"
echo "ok delta required but don't exist"

repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
if ${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin ${new_rev} 2>err.txt; then
    assert_not_reached "--require-static-deltas unexpectedly succeeded"
fi
assert_file_has_content err.txt "deltas required, but none found"
echo "ok delta required for revision"

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

cd ${test_tmpdir}
repo_init
${CMD_PREFIX} ostree --repo=repo remote add origin-bad $(cat httpd-address)/ostree/noent
if ${CMD_PREFIX} ostree --repo=repo --depth=0 pull origin-bad main 2>err.txt; then
    assert_not_reached "pull repo 404 succeeded?"
fi
assert_file_has_content err.txt "404"
echo "ok pull repo 404"

cd ${test_tmpdir}
repo_init --set=gpg-verify=true
if ${CMD_PREFIX} ostree --repo=repo --depth=0 pull origin main 2>err.txt; then
    assert_not_reached "pull repo 404 succeeded?"
fi
assert_file_has_content err.txt "GPG verification enabled, but no signatures found"
echo "ok pull repo 404 (gpg)"

cd ${test_tmpdir}
repo_init --set=gpg-verify=true
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit \
  --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1} -b main \
  -s "A signed commit" --tree=ref=main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
# make sure gpg verification is correctly on
csum=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo rev-parse main)
objpath=objects/${csum::2}/${csum:2}.commitmeta
remotesig=ostree-srv/gnomerepo/$objpath
localsig=repo/$objpath
mv $remotesig $remotesig.bak
if ${CMD_PREFIX} ostree --repo=repo --depth=0 pull origin main; then
    assert_not_reached "pull with gpg-verify unexpectedly succeeded?"
fi
# ok now check that we can pull correctly
mv $remotesig.bak $remotesig
${CMD_PREFIX} ostree --repo=repo pull origin main
echo "ok pull signed commit"
rm $localsig
${CMD_PREFIX} ostree --repo=repo pull origin main
test -f $localsig
echo "ok re-pull signature for stored commit"
