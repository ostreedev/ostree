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

echo "1..48"

$OSTREE checkout test2 checkout-test2
echo "ok checkout"

$OSTREE rev-parse test2
$OSTREE rev-parse 'test2^'
$OSTREE rev-parse 'test2^^' 2>/dev/null && (echo 1>&2 "rev-parse test2^^ unexpectedly succeeded!"; exit 1)
echo "ok rev-parse"

checksum=$($OSTREE rev-parse test2)
partial=${checksum:0:6} 
echo "partial:" $partial
echo "corresponds to:" $checksum
$OSTREE rev-parse test2 > checksum
$OSTREE rev-parse $partial > partial-results
assert_file_has_content checksum $(cat partial-results)
echo "ok shortened checksum"

(cd repo && ${CMD_PREFIX} ostree rev-parse test2)
echo "ok repo-in-cwd"

$OSTREE refs > reflist
assert_file_has_content reflist '^test2$'
rm reflist

$OSTREE refs --delete 2>/dev/null && (echo 1>&2 "refs --delete (without prefix) unexpectedly succeeded!"; exit 1)

echo "ok refs"

cd checkout-test2
assert_has_file firstfile
assert_has_file baz/cow
assert_file_has_content baz/cow moo
assert_has_file baz/deeper/ohyeah
echo "ok content"

rm firstfile
$OSTREE commit -b test2 -s delete

cd $test_tmpdir
$OSTREE checkout test2 $test_tmpdir/checkout-test2-2
cd $test_tmpdir/checkout-test2-2
assert_not_has_file firstfile
assert_has_file baz/saucer
echo "ok removal"

mkdir -p a/nested/tree
echo one > a/nested/tree/1
echo two2 > a/nested/2
echo 3 > a/nested/3
touch a/4
echo fivebaby > a/5
touch a/6
echo whee > 7
mkdir -p another/nested/tree
echo anotherone > another/nested/tree/1
echo whee2 > another/whee
# FIXME - remove grep for .
$OSTREE commit -b test2 -s "Another commit"
echo "ok commit"

cd ${test_tmpdir}
$OSTREE checkout test2 $test_tmpdir/checkout-test2-3
cd checkout-test2-3
assert_has_file a/nested/2
assert_file_has_content a/nested/2 'two2'
echo "ok stdin contents"

cd ${test_tmpdir}/checkout-test2-3
echo 4 > four
mkdir -p yet/another/tree
echo leaf > yet/another/tree/green
echo helloworld > yet/message
rm a/5
$OSTREE commit -b test2 -s "Current directory"
echo "ok cwd commit"

cd ${test_tmpdir}
$OSTREE checkout test2 $test_tmpdir/checkout-test2-4
cd checkout-test2-4
assert_file_has_content yet/another/tree/green 'leaf'
assert_file_has_content four '4'
echo "ok cwd contents"

cd ${test_tmpdir}
$OSTREE diff test2^ test2 > diff-test2
assert_file_has_content diff-test2 'D */a/5'
assert_file_has_content diff-test2 'A */yet$'
assert_file_has_content diff-test2 'A */yet/message$'
assert_file_has_content diff-test2 'A */yet/another/tree/green$'
echo "ok diff revisions"

cd ${test_tmpdir}/checkout-test2-4
echo afile > oh-look-a-file
$OSTREE diff test2 ./ > ${test_tmpdir}/diff-test2-2
rm oh-look-a-file
cd ${test_tmpdir}
assert_file_has_content diff-test2-2 'A *oh-look-a-file$'
echo "ok diff cwd"

cd ${test_tmpdir}/checkout-test2-4
rm four
mkdir four
touch four/other
$OSTREE diff test2 ./ > ${test_tmpdir}/diff-test2-2
cd ${test_tmpdir}
assert_file_has_content diff-test2-2 'M */four$'
echo "ok diff file changing type"

cd ${test_tmpdir}
mkdir repo2
${CMD_PREFIX} ostree --repo=repo2 init
${CMD_PREFIX} ostree --repo=repo2 pull-local repo
echo "ok pull-local"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo2 checkout test2 test2-checkout-from-local-clone
cd test2-checkout-from-local-clone
assert_file_has_content yet/another/tree/green 'leaf'
echo "ok local clone checkout"

$OSTREE checkout -U test2 checkout-user-test2
echo "ok user checkout"

$OSTREE commit -b test2 -s "Another commit" --tree=ref=test2
echo "ok commit from ref"

$OSTREE commit -b trees/test2 -s 'ref with / in it' --tree=ref=test2
echo "ok commit ref with /"

old_rev=$($OSTREE rev-parse test2)
$OSTREE commit --skip-if-unchanged -b test2 -s 'should not be committed' --tree=ref=test2
new_rev=$($OSTREE rev-parse test2)
assert_streq "${old_rev}" "${new_rev}"
echo "ok commit --skip-if-unchanged"

cd ${test_tmpdir}/checkout-test2-4
$OSTREE commit -b test2 -s "no xattrs" --no-xattrs
echo "ok commit with no xattrs"

cd ${test_tmpdir}
cat > test-statoverride.txt <<EOF
+2048 /a/nested/3
EOF
cd ${test_tmpdir}/checkout-test2-4
$OSTREE commit -b test2 -s "with statoverride" --statoverride=../test-statoverride.txt
echo "ok commit statoverridde"

cd ${test_tmpdir}
$OSTREE prune
echo "ok prune didn't fail"

cd ${test_tmpdir}
$OSTREE cat test2 /yet/another/tree/green > greenfile-contents
assert_file_has_content greenfile-contents "leaf"
echo "ok cat-file"

cd ${test_tmpdir}
$OSTREE checkout --subpath /yet/another test2 checkout-test2-subpath
cd checkout-test2-subpath
assert_file_has_content tree/green "leaf"
echo "ok checkout subpath"

cd ${test_tmpdir}
$OSTREE checkout --union test2 checkout-test2-union
find checkout-test2-union | wc -l > union-files-count
$OSTREE checkout --union test2 checkout-test2-union
find checkout-test2-union | wc -l > union-files-count.new
cmp union-files-count{,.new}
cd checkout-test2-union
assert_file_has_content ./yet/another/tree/green "leaf"
echo "ok checkout union 1"

cd ${test_tmpdir}
rm -rf shadow-repo
mkdir shadow-repo
${CMD_PREFIX} ostree --repo=shadow-repo init
${CMD_PREFIX} ostree --repo=shadow-repo config set core.parent $(pwd)/repo
rm -rf test2-checkout
parent_rev_test2=$(${CMD_PREFIX} ostree --repo=repo rev-parse test2)
${CMD_PREFIX} ostree --repo=shadow-repo checkout "${parent_rev_test2}" test2-checkout
echo "ok checkout from shadow repo"

cd ${test_tmpdir}
if $OSTREE checkout test2 --subpath /enoent 2>err.txt; then
    assert_not_reached "checking outnonexistent file unexpectedly succeeded!"
fi
assert_file_has_content err.txt 'No such file or directory'
echo "ok subdir enoent"

cd ${test_tmpdir}
$OSTREE checkout test2 --allow-noent --subpath /enoent 2>/dev/null
echo "ok subdir noent"

cd ${test_tmpdir}
mkdir repo3
${CMD_PREFIX} ostree --repo=repo3 init
${CMD_PREFIX} ostree --repo=repo3 pull-local --remote=aremote repo test2
${CMD_PREFIX} ostree --repo=repo3 rev-parse aremote/test2
echo "ok pull-local with --remote arg"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo3 prune
find repo3/objects -name '*.commit' > objlist-before-prune
rm repo3/refs/heads/* repo3/refs/remotes/* -rf
${CMD_PREFIX} ostree --repo=repo3 prune --refs-only
find repo3/objects -name '*.commit' > objlist-after-prune
if cmp -s objlist-before-prune objlist-after-prune; then
    echo "Prune didn't delete anything!"; exit 1
fi
rm repo3 objlist-before-prune objlist-after-prune -rf
echo "ok prune"

cd ${test_tmpdir}
rm repo3 -rf
${CMD_PREFIX} ostree --repo=repo3 init --mode=archive-z2
${CMD_PREFIX} ostree --repo=repo3 pull-local --remote=aremote repo test2
rm repo3/refs/remotes -rf
mkdir repo3/refs/remotes
${CMD_PREFIX} ostree --repo=repo3 prune --refs-only
find repo3/objects -name '*.filez' > file-objects
if test -s file-objects; then
    assert_not_reached "prune didn't delete all objects"
fi
echo "ok prune in archive-z2 deleted everything"

cd ${test_tmpdir}
$OSTREE commit -b test3 -s "Another commit" --tree=ref=test2
${CMD_PREFIX} ostree --repo=repo refs > reflist
assert_file_has_content reflist '^test3$'
${CMD_PREFIX} ostree --repo=repo refs --delete test3
${CMD_PREFIX} ostree --repo=repo refs > reflist
assert_not_file_has_content reflist '^test3$'
echo "ok reflist --delete"

cd ${test_tmpdir}
rm -rf test2-checkout
$OSTREE checkout test2 test2-checkout
(cd test2-checkout && $OSTREE commit --link-checkout-speedup -b test2 -s "tmp")
echo "ok commit with link speedup"

cd ${test_tmpdir}
$OSTREE ls test2
echo "ok ls with no argument"

cd ${test_tmpdir}
if $OSTREE ls test2 /baz/cow/notadir 2>errmsg; then
    assert_not_reached
fi
assert_file_has_content errmsg "Not a directory"
echo "ok ls of not a directory"

cd ${test_tmpdir}
$OSTREE show test2
echo "ok show with non-checksum"

cd $test_tmpdir/checkout-test2
checksum=$($OSTREE commit -b test4 -s "Third commit")
cd ${test_tmpdir}
$OSTREE show test4 > show-output
assert_file_has_content show-output "Third commit"
assert_file_has_content show-output "commit $checksum"
echo "ok show full output"

cd $test_tmpdir/checkout-test2
checksum1=$($OSTREE commit -b test5 -s "First commit")
checksum2=$($OSTREE commit -b test5 -s "Second commit")
cd ${test_tmpdir}
$OSTREE log test5 > log-output
assert_file_has_content log-output "First commit"
assert_file_has_content log-output "commit $checksum1"
assert_file_has_content log-output "Second commit"
assert_file_has_content log-output "commit $checksum2"
echo "ok log output"

cd $test_tmpdir/checkout-test2
checksum1=$($OSTREE commit -b test6 -s "First commit")
checksum2=$($OSTREE commit -b test6 -s "Second commit")
cd ${test_tmpdir}
$OSTREE show test6 > show-output
assert_file_has_content show-output "commit $checksum2"
$OSTREE reset test6 $checksum1
$OSTREE show test6 > show-output
assert_file_has_content show-output "commit $checksum1"
echo "ok basic reset"

cd ${test_tmpdir}
rm checkout-test2 -rf
$OSTREE checkout test2 checkout-test2
touch checkout-test2/sometestfile
$OSTREE commit -s sometest -b test2 checkout-test2
echo "ok commit with directory filename"

cd $test_tmpdir/checkout-test2
$OSTREE commit -b test2 -s "Metadata string" --add-metadata-string=FOO=BAR --add-metadata-string=KITTENS=CUTE --add-detached-metadata-string=SIGNATURE=HANCOCK --tree=ref=test2
cd ${test_tmpdir}
$OSTREE show --print-metadata-key=FOO test2 > test2-meta
assert_file_has_content test2-meta "BAR"
$OSTREE show --print-metadata-key=KITTENS test2 > test2-meta
assert_file_has_content test2-meta "CUTE"
$OSTREE show --print-detached-metadata-key=SIGNATURE test2 > test2-meta
assert_file_has_content test2-meta "HANCOCK"
echo "ok metadata commit with strings"

cd ${test_tmpdir}
rm repo2 -rf
mkdir repo2
${CMD_PREFIX} ostree --repo=repo2 init
${CMD_PREFIX} ostree --repo=repo2 pull-local repo
${CMD_PREFIX} ostree --repo=repo2 show --print-detached-metadata-key=SIGNATURE test2 > test2-meta
assert_file_has_content test2-meta "HANCOCK"
echo "ok pull-local after commit metadata"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo remote --set=tls-permissive=true add aremote http://remote.example.com/repo testos/buildmaster/x86_64-runtime
assert_file_has_content repo/config 'tls-permissive=true'
assert_file_has_content repo/config 'remote\.example\.com'
echo "ok remote add with set"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo remote show-url aremote > aremote-url.txt
assert_file_has_content aremote-url.txt 'http.*remote\.example\.com/repo'
echo "ok remote show-url"

cd ${test_tmpdir}
rm -rf test2-checkout
if grep bare-user repo/config; then
    $OSTREE checkout -U test2 test2-checkout
else
    $OSTREE checkout test2 test2-checkout
fi
stat '--format=%Y' test2-checkout/baz/cow > cow-mtime
assert_file_has_content cow-mtime 0
stat '--format=%Y' test2-checkout/baz/deeper > deeper-mtime
assert_file_has_content deeper-mtime 0
echo "ok content mtime"

cd ${test_tmpdir}
rm -rf test2-checkout
mkdir -p test2-checkout
cd test2-checkout
mkfifo afifo
if $OSTREE commit -b test2 -s "Attempt to commit a FIFO" 2>../errmsg; then
    assert_not_reached "Committing a FIFO unexpetedly succeeded!"
    assert_file_has_content ../errmsg "Unsupported file type"
fi
echo "ok commit of fifo was rejected"

cd ${test_tmpdir}
rm repo2 -rf
mkdir repo2
${CMD_PREFIX} ostree --repo=repo2 init --mode=archive-z2
${CMD_PREFIX} ostree --repo=repo2 pull-local repo
rm -rf test2-checkout
${CMD_PREFIX} ostree --repo=repo2 checkout -U --disable-cache test2 test2-checkout
if test -d repo2/uncompressed-objects-cache; then
    ls repo2/uncompressed-objects-cache > ls.txt
    if test -s ls.txt; then
	assert_not_reached "repo has uncompressed objects"
    fi
fi
rm test2-checkout -rf
${CMD_PREFIX} ostree --repo=repo2 checkout -U test2 test2-checkout
assert_file_has_content test2-checkout/baz/cow moo
assert_has_dir repo2/uncompressed-objects-cache
echo "ok disable cache checkout"

# Whiteouts
cd ${test_tmpdir}
mkdir -p overlay/baz/
touch overlay/baz/.wh.cow
touch overlay/.wh.deeper
touch overlay/anewfile
mkdir overlay/anewdir/
touch overlay/anewdir/blah
$OSTREE --repo=repo commit -b overlay -s 'overlay' --tree=dir=overlay
rm overlay -rf

for branch in test2 overlay; do
    $OSTREE --repo=repo checkout --union --whiteouts ${branch} overlay-co
done
for f in .wh.deeper baz/cow baz/.wh.cow; do
    assert_not_has_file overlay-co/${f}
done
assert_not_has_dir overlay-co/deeper
assert_has_file overlay-co/anewdir/blah
assert_has_file overlay-co/anewfile

echo "ok whiteouts enabled"

# Now double check whiteouts are not processed without --whiteouts 
rm overlay-co -rf
for branch in test2 overlay; do
    $OSTREE --repo=repo checkout --union ${branch} overlay-co
done
for f in .wh.deeper baz/cow baz/.wh.cow; do
    assert_has_file overlay-co/${f}
done
assert_not_has_dir overlay-co/deeper
assert_has_file overlay-co/anewdir/blah
assert_has_file overlay-co/anewfile
echo "ok whiteouts disabled"

cd ${test_tmpdir}
rm -rf test2-checkout
mkdir -p test2-checkout
cd test2-checkout
touch should-not-be-fsynced
$OSTREE commit -b test2 -s "Unfsynced commit" --fsync=false

# Run this test only as non-root user.  When run as root, the chmod
# won't have any effect.
if test "$(id -u)" != "0"; then
    cd ${test_tmpdir}
    rm -f expected-fail error-message
    $OSTREE init --mode=archive-z2 --repo=repo-noperm
    chmod -w repo-noperm/objects
    $OSTREE --repo=repo-noperm pull-local repo 2> error-message || touch expected-fail
    chmod +w repo-noperm/objects
    assert_has_file expected-fail
    assert_file_has_content error-message "Permission denied"
    echo "ok unwritable repo was caught"
fi

cd ${test_tmpdir}
rm -rf test2-checkout
mkdir -p test2-checkout
cd test2-checkout
touch blah
stat --printf="%Z\n" ${test_tmpdir}/repo > ${test_tmpdir}/timestamp-orig.txt
$OSTREE commit -b test2 -s "Should bump the mtime"
stat --printf="%Z\n" ${test_tmpdir}/repo > ${test_tmpdir}/timestamp-new.txt
cd ..
if ! cmp timestamp-{orig,new}.txt; then
    assert_not_reached "failed to update mtime on repo"
fi
