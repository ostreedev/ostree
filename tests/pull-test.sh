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
    ostree_repo_init repo --mode=${repo_mode}
    ${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo "$@"
}

repo_init --no-gpg-verify

# See also the copy of this in basic-test.sh
COMMIT_ARGS=""
CHECKOUT_U_ARG=""
CHECKOUT_H_ARGS="-H"
if is_bare_user_only_repo repo; then
    COMMIT_ARGS="--canonical-permissions"
    # Also, since we can't check out uid=0 files we need to check out in user mode
    CHECKOUT_U_ARG="-U"
    CHECKOUT_H_ARGS="-U -H"
else
    if grep -E -q '^mode=bare-user' repo/config; then
        CHECKOUT_H_ARGS="-U -H"
    fi
fi

function verify_initial_contents() {
    rm checkout-origin-main -rf
    $OSTREE checkout ${CHECKOUT_H_ARGS} origin/main checkout-origin-main
    cd checkout-origin-main
    assert_file_has_content firstfile '^first$'
    assert_file_has_content baz/cow '^moo$'
}

echo "1..33"

# Try both syntaxes
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main >out.txt
assert_file_has_content out.txt "[1-9][0-9]* metadata, [1-9][0-9]* content objects fetched"
${CMD_PREFIX} ostree --repo=repo pull origin:main > out.txt
assert_not_file_has_content out.txt "[1-9][0-9]* content objects fetched"
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok pull"

cd ${test_tmpdir}
verify_initial_contents
echo "ok pull contents"

cd ${test_tmpdir}
mkdir mirrorrepo
ostree_repo_init mirrorrepo --mode=archive
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
$OSTREE show main >/dev/null
echo "ok pull mirror"

mkdir otherbranch
echo someothercontent > otherbranch/someothercontent
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b otherbranch --tree=dir=otherbranch
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
rm mirrorrepo -rf
# All refs
ostree_repo_init mirrorrepo --mode=archive
${CMD_PREFIX} ostree --repo=mirrorrepo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror origin
${CMD_PREFIX} ostree --repo=mirrorrepo fsck
for ref in main otherbranch; do
    ${CMD_PREFIX} ostree --repo=mirrorrepo rev-parse $ref
done
echo "ok pull mirror (all refs)"

rm mirrorrepo -rf
ostree_repo_init mirrorrepo --mode=archive
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
$OSTREE --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main checkout-origin-main
echo moomoo > checkout-origin-main/baz/cow
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s "" --tree=dir=checkout-origin-main
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
$OSTREE --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main checkout-origin-main
cat > statoverride.txt <<EOF
2048 /some-setuid
EOF
echo asetuid > checkout-origin-main/some-setuid
# Don't use ${COMMIT_ARGS} as we don't want --canonical-permissions with bare-user-only
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b content-with-suid --statoverride=statoverride.txt --tree=dir=checkout-origin-main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
# Verify we reject it both when unpacking and when mirroring
for flag in "" "--mirror"; do
    if ${CMD_PREFIX} ostree --repo=mirrorrepo pull ${flag} --bareuseronly-files origin content-with-suid 2>err.txt; then
        assert_not_reached "pulled unsafe bareuseronly"
    fi
    assert_file_has_content err.txt 'Content object.*: invalid mode.*with bits 040.*'
done
echo "ok pull (bareuseronly, unsafe)"

cd ${test_tmpdir}
rm mirrorrepo/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=mirrorrepo prune --refs-only
${CMD_PREFIX} ostree --repo=mirrorrepo pull --mirror --bareuseronly-files origin main
echo "ok pull (bareuseronly mirror)"

# Corruption tests <https://github.com/ostreedev/ostree/issues/1211>
cd ${test_tmpdir}
repo_init --no-gpg-verify
if ! is_bare_user_only_repo repo; then
if ! skip_one_without_user_xattrs; then
    if is_bare_user_only_repo repo; then
        cacherepomode=bare-user-only
    else
        cacherepomode=bare-user
    fi
    rm cacherepo -rf
    ostree_repo_init cacherepo --mode=${cacherepomode}
    ${CMD_PREFIX} ostree --repo=cacherepo pull-local ostree-srv/gnomerepo main
    rev=$(ostree --repo=cacherepo rev-parse main)
    ${CMD_PREFIX} ostree --repo=cacherepo ls -R -C main > ls.txt
    regfile_hash=$(grep -E -e '^-0' ls.txt | head -1 | awk '{ print $5 }')
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false corruptrepo $(cat httpd-address)/ostree/corruptrepo
    # Make this a loop so in the future we can add more object types like commit etc.
    for object in ${regfile_hash}.file; do
        checksum=$(echo ${object} | sed -e 's,\(.*\)\.[a-z]*$,\1,')
        path=cacherepo/objects/${object:0:2}/${object:2}
        # Preserve user.ostreemeta xattr
        cp -a ${path}{,.new}
        (dd if=${path} conv=swab) > ${path}.new
        mv ${path}{.new,}
        if ${CMD_PREFIX} ostree --repo=cacherepo fsck 2>err.txt; then
            fatal "corrupt repo fsck?"
        fi
        assert_file_has_content err.txt "corrupted.*${checksum}"
        rm ostree-srv/corruptrepo -rf
        ostree_repo_init ostree-srv/corruptrepo --mode=archive
        ${CMD_PREFIX} ostree --repo=ostree-srv/corruptrepo pull-local cacherepo main
        # Pulling via HTTP into a non-archive should fail, even with
        # --http-trusted.
        if ${CMD_PREFIX} ostree --repo=repo pull --http-trusted corruptrepo main 2>err.txt; then
            fatal "Pulled from corrupt repo?"
        fi
        assert_file_has_content err.txt "Corrupted.*${checksum}"
        if ${CMD_PREFIX} ostree --repo=repo show corruptrepo:main >/dev/null; then
            fatal "Pulled from corrupt repo?"
        fi
        ${CMD_PREFIX} ostree --repo=repo prune --refs-only
        rm repo/tmp/* -rf
        ostree_repo_init corruptmirrorrepo --mode=archive
        # Pulling via http-trusted should not verify the checksum
        ${CMD_PREFIX} ostree --repo=corruptmirrorrepo remote add --set=gpg-verify=false corruptrepo $(cat httpd-address)/ostree/corruptrepo
        ${CMD_PREFIX} ostree --repo=corruptmirrorrepo pull --mirror --http-trusted corruptrepo main
        # But it should fail to fsck
        if ${CMD_PREFIX} ostree --repo=corruptmirrorrepo fsck 2>err.txt; then
            fatal "corrupt mirror repo fsck?"
        fi
    done

    # And ensure the repo is reinitialized
    repo_init --no-gpg-verify
    echo "ok corruption"
fi
else
# bareuseronly case, we don't mark it as SKIP at the moment
echo "ok corruption (skipped)"
fi

cd ${test_tmpdir}
rm mirrorrepo/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=mirrorrepo prune --refs-only
${CMD_PREFIX} ostree --repo=mirrorrepo pull origin main
rm checkout-origin-main -rf
$OSTREE --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main checkout-origin-main
echo yetmorecontent > checkout-origin-main/baz/cowtest
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s "" --tree=dir=checkout-origin-main
rev=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo rev-parse main)
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
${CMD_PREFIX} ostree --repo=mirrorrepo pull --commit-metadata-only origin main
assert_has_file mirrorrepo/state/${rev}.commitpartial
echo "ok pull commit metadata only (should not apply deltas)"

cd ${test_tmpdir}
mkdir mirrorrepo-local
ostree_repo_init mirrorrepo-local --mode=archive
${CMD_PREFIX} ostree --repo=mirrorrepo-local remote add --set=gpg-verify=false origin file://$(pwd)/ostree-srv/gnomerepo
${CMD_PREFIX} ostree --repo=mirrorrepo-local pull --mirror origin main
${CMD_PREFIX} ostree --repo=mirrorrepo-local fsck
${CMD_PREFIX} ostree --repo=mirrorrepo show main >/dev/null
echo "ok pull local mirror"

cd ${test_tmpdir}
# This is more of a known issue; test that we give a clean error right now
rm otherrepo -rf
ostree_repo_init otherrepo --mode=archive
rm checkout-origin-main -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main checkout-origin-main
${CMD_PREFIX} ostree --repo=otherrepo commit ${COMMIT_ARGS} -b localbranch --tree=dir=checkout-origin-main
${CMD_PREFIX} ostree --repo=otherrepo remote add --set=gpg-verify=false origin file://$(pwd)/ostree-srv/gnomerepo
${CMD_PREFIX} ostree --repo=otherrepo pull origin main
rm mirrorrepo-local -rf
ostree_repo_init mirrorrepo-local --mode=archive
if ${CMD_PREFIX} ostree --repo=mirrorrepo-local pull-local otherrepo 2>err.txt; then
    fatal "pull with mixed refs succeeded?"
fi
assert_file_has_content err.txt "error: Invalid ref name origin:main"
${CMD_PREFIX} ostree --repo=mirrorrepo-local pull-local otherrepo localbranch
${CMD_PREFIX} ostree --repo=mirrorrepo-local rev-parse localbranch
${CMD_PREFIX} ostree --repo=mirrorrepo-local fsck
echo "ok pull-local mirror errors with mixed refs"

rm -f otherrepo/summary
if ${CMD_PREFIX} ostree --repo=mirrorrepo-local pull-local otherrepo nosuchbranch 2>err.txt; then
    fatal "pulled nonexistent branch"
fi
# So true
assert_file_has_content_literal err.txt "error: Refspec 'nosuchbranch' not found"
echo "ok pull-local nonexistent branch"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s "Metadata string" --add-detached-metadata-string=SIGNATURE=HANCOCK --tree=ref=main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
$OSTREE show --print-detached-metadata-key=SIGNATURE main > main-meta
assert_file_has_content main-meta "HANCOCK"
echo "ok pull detached metadata"

cd ${test_tmpdir}
mkdir parentpullrepo
ostree_repo_init parentpullrepo --mode=archive
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

# test pull -T
cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main
origrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse main)
# Check we can pull the same commit with timestamp checking enabled
${CMD_PREFIX} ostree --repo=repo pull -T origin main
assert_streq ${origrev} "$(${CMD_PREFIX} ostree --repo=repo rev-parse main)"
newrev=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main --tree=ref=main)
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
# New commit with timestamp checking
${CMD_PREFIX} ostree --repo=repo pull -T origin main
assert_not_streq "${origrev}" "${newrev}"
assert_streq ${newrev} "$(${CMD_PREFIX} ostree --repo=repo rev-parse main)"
newrev2=$(${CMD_PREFIX} ostree --timestamp="October 25 1985" --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main --tree=ref=main)
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u
if ${CMD_PREFIX} ostree --repo=repo pull -T origin main 2>err.txt; then
    fatal "pulled older commit with timestamp checking enabled?"
fi
assert_file_has_content err.txt "Upgrade.*is chronologically older"
assert_streq ${newrev} "$(${CMD_PREFIX} ostree --repo=repo rev-parse main)"
# But we can pull it without timestamp checking
${CMD_PREFIX} ostree --repo=repo pull origin main
echo "ok pull timestamp checking"

cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck
# Generate a delta from old to current, even though we aren't going to
# use it.
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main

rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main main-files
cd main-files
echo "an added file for static deltas" > added-file
echo "modified file for static deltas" > baz/cow
rm baz/saucer
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s 'static delta test'
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
delta_dry_run_regexp='Delta update: 0/1 parts, 0 bytes/[456][0-9][0-9] bytes, 455 bytes total uncompressed'
assert_file_has_content dry-run-pull.txt "${delta_dry_run_regexp}"
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_streq "${prev_rev}" "${rev}"
${CMD_PREFIX} ostree --repo=repo fsck
done

# Test pull via file:/// - this should still use the deltas path for testing
cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo remote delete origin
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin file://$(pwd)/ostree-srv/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --dry-run --require-static-deltas origin ${delta_target} >dry-run-pull.txt
# See above
assert_file_has_content dry-run-pull.txt "${delta_dry_run_regexp}"
echo "ok pull file:// + deltas required"

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

# Test no-op with deltas: https://github.com/ostreedev/ostree/issues/1321
cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin main

cd ${test_tmpdir}
repo_init --no-gpg-verify
${CMD_PREFIX} ostree --repo=repo pull origin main@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --disable-static-deltas origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout ${CHECKOUT_H_ARGS} origin:main checkout-origin-main
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
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main main-files
cd main-files
echo "more added files for static deltas" > added-file2
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s 'inline static delta test'
cd ..
rm main-files -rf
# Generate new delta that we'll use
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate --inline main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout ${CHECKOUT_H_ARGS} origin:main checkout-origin-main
cd checkout-origin-main
assert_file_has_content added-file2 "more added files for static deltas"

echo "ok inline static delta"

cd ${test_tmpdir}
rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout ${CHECKOUT_U_ARG} main main-files
cd main-files
# Make a file larger than 16M for testing
dd if=/dev/zero of=test-bigfile count=1 seek=42678
echo "further modified file for static deltas" > baz/cow
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit ${COMMIT_ARGS} -b main -s '2nd static delta test'
cd ..
rm main-files -rf
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo fsck

rm checkout-origin-main -rf
$OSTREE checkout ${CHECKOUT_H_ARGS} origin:main checkout-origin-main
cd checkout-origin-main
assert_has_file test-bigfile
stat --format=%s test-bigfile > bigfile-size
assert_file_has_content bigfile-size 21851648
assert_file_has_content baz/cow "further modified file for static deltas"
assert_not_has_file baz/saucer

echo "ok static delta 2"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo pull origin main main@${rev} main@${rev} main main@${rev} main 
echo "ok pull specific commit array"

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
find ostree-srv/gnomerepo/objects -name '*.dirtree' | while read f; do mv ${f}{,.orig}; done
repo_init --set=gpg-verify=false
if ${CMD_PREFIX} ostree --repo=repo --depth=0 pull origin main 2>err.txt; then
    assert_not_reached "pull repo 404 succeeded?"
fi
assert_file_has_content err.txt "404"
find ostree-srv/gnomerepo/objects -name '*.dirtree.orig' | while read f; do mv ${f} $(dirname $f)/$(basename ${f} .orig); done
echo "ok pull repo 404 on dirtree object"

cd ${test_tmpdir}
repo_init --set=gpg-verify=true
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit ${COMMIT_ARGS} \
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

cd ${test_tmpdir}
repo_init --no-gpg-verify
mv ostree-srv/gnomerepo/refs/heads/main{,.orig}
rm ostree-srv/gnomerepo/summary
(for x in $(seq 20); do echo "lots of html here "; done) > ostree-srv/gnomerepo/refs/heads/main
if ${CMD_PREFIX} ostree --repo=repo pull origin main 2>err.txt; then
    fatal "pull of invalid ref succeeded"
fi
assert_file_has_content_literal err.txt 'error: Fetching checksum for ref ((empty), main): Invalid rev lots of html here  lots of html here  lots of html here  lots of'
echo "ok pull got HTML for a ref"
