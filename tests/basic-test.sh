# This file is to be sourced, not executed

# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

set -euo pipefail

echo "1..$((82 + ${extra_basic_tests:-0}))"

CHECKOUT_U_ARG=""
CHECKOUT_H_ARGS="-H"
COMMIT_ARGS=""
DIFF_ARGS=""
if is_bare_user_only_repo repo; then
    # In bare-user-only repos we can only represent files with uid/gid 0, no
    # xattrs and canonical permissions, so we need to commit them as such, or
    # we end up with repos that don't pass fsck
    COMMIT_ARGS="--canonical-permissions"
    DIFF_ARGS="--owner-uid=0 --owner-gid=0 --no-xattrs"
    # Also, since we can't check out uid=0 files we need to check out in user mode
    CHECKOUT_U_ARG="-U"
    CHECKOUT_H_ARGS="-U -H"
else
    if grep -E -q '^mode=bare-user' repo/config; then
        CHECKOUT_H_ARGS="-U -H"
    fi
fi

# This should be dynamic now
assert_not_has_dir repo/uncompressed-objects-cache

validate_checkout_basic() {
    (cd $1;
     assert_has_file firstfile
     assert_has_file baz/cow
     assert_file_has_content baz/cow moo
     assert_has_file baz/deeper/ohyeah
     assert_symlink_has_content somelink nosuchfile
     )
}

$OSTREE checkout test2 checkout-test2
validate_checkout_basic checkout-test2
if grep -q 'mode=bare$' repo/config; then
    assert_not_streq $(stat -c '%h' checkout-test2/firstfile) 1
fi
echo "ok checkout"

# Note this tests bare-user *and* bare-user-only
rm checkout-test2 -rf
if grep -q bare-user repo/config; then
    $OSTREE checkout -U -H test2 checkout-test2
else
    $OSTREE checkout -H test2 checkout-test2
fi
validate_checkout_basic checkout-test2
rm checkout-test2 -rf
# Only do these tests on bare-user/bare, not bare-user-only
# since the latter automatically synthesizes -U if it's not passed.
if ! is_bare_user_only_repo repo; then
if grep -q bare-user repo/config; then
    if $OSTREE checkout -H test2 checkout-test2 2>err.txt; then
        assert_not_reached "checkout -H worked?"
    fi
    assert_file_has_content err.txt "User repository.*requires.*user"
else
    if $OSTREE checkout -U -H test2 checkout-test2 2>err.txt; then
        assert_not_reached "checkout -H worked?"
    fi
    assert_file_has_content err.txt "Bare repository mode cannot hardlink in user"
fi
fi
echo "ok checkout -H"

rm checkout-test2 -rf
$OSTREE checkout -C test2 checkout-test2
for file in firstfile baz/cow baz/alink; do
    assert_streq $(stat -c '%h' checkout-test2/$file) 1
done

echo "ok checkout -C"

$OSTREE rev-parse test2
$OSTREE rev-parse 'test2^'
$OSTREE rev-parse 'test2^^' 2>/dev/null && fatal "rev-parse test2^^ unexpectedly succeeded!"
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

if ! skip_one_without_user_xattrs; then
    rm test-repo -rf
    ostree_repo_init test-repo --mode=bare-user
    ostree_repo_init test-repo --mode=bare-user
    rm test-repo -rf
    echo "ok repo-init on existing repo"
fi

if ! skip_one_without_user_xattrs; then
    rm test-repo -rf
    ostree_repo_init test-repo --mode=bare-user
    ${CMD_PREFIX} ostree --repo=test-repo refs
    rm -rf test-repo/tmp
    ${CMD_PREFIX} ostree --repo=test-repo refs
    assert_has_dir test-repo/tmp
    echo "ok autocreate tmp"
fi

rm checkout-test2 -rf
$OSTREE checkout test2 checkout-test2
cd checkout-test2
rm firstfile
$OSTREE commit ${COMMIT_ARGS} -b test2 -s delete

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
$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Another commit"
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
$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Current directory"
echo "ok cwd commit"

cd ${test_tmpdir}
$OSTREE checkout test2 $test_tmpdir/checkout-test2-4
cd checkout-test2-4
assert_file_has_content yet/another/tree/green 'leaf'
assert_file_has_content four '4'
echo "ok cwd contents"

cd ${test_tmpdir}
rm checkout-test2-l -rf
$OSTREE checkout ${CHECKOUT_H_ARGS} test2 $test_tmpdir/checkout-test2-l
date > $test_tmpdir/checkout-test2-l/newdatefile.txt
$OSTREE commit ${COMMIT_ARGS} --link-checkout-speedup --consume -b test2 --tree=dir=$test_tmpdir/checkout-test2-l
assert_not_has_dir $test_tmpdir/checkout-test2-l
$OSTREE fsck
# Some of the later tests are sensitive to state
$OSTREE reset test2 test2^
$OSTREE prune --refs-only
echo "ok consume (nom nom nom)"

# Test adopt
cd ${test_tmpdir}
rm checkout-test2-l -rf
$OSTREE checkout ${CHECKOUT_H_ARGS} test2 $test_tmpdir/checkout-test2-l
echo 'a file to consume 🍔' > $test_tmpdir/checkout-test2-l/eatme.txt
# Save a link to it for device/inode comparison
ln $test_tmpdir/checkout-test2-l/eatme.txt $test_tmpdir/eatme-savedlink.txt
$OSTREE commit ${COMMIT_ARGS} --link-checkout-speedup --consume -b test2 --tree=dir=$test_tmpdir/checkout-test2-l
$OSTREE fsck
# Adoption isn't implemented for bare-user yet
eatme_objpath=$(ostree_file_path_to_object_path repo test2 /eatme.txt)
if grep -q '^mode=bare$' repo/config || is_bare_user_only_repo repo; then
    assert_files_hardlinked ${test_tmpdir}/eatme-savedlink.txt ${eatme_objpath}
else
    if files_are_hardlinked ${test_tmpdir}/eatme-savedlink.txt ${eatme_objpath}; then
        fatal "bare-user adopted?"
    fi
fi
assert_not_has_dir $test_tmpdir/checkout-test2-l
# Some of the later tests are sensitive to state
$OSTREE reset test2 test2^
$OSTREE prune --refs-only
rm -f ${test_tmpdir}/eatme-savedlink.txt
echo "ok adopt"

cd ${test_tmpdir}
$OSTREE commit ${COMMIT_ARGS} -b test2-no-parent -s '' $test_tmpdir/checkout-test2-4
assert_streq $($OSTREE log test2-no-parent |grep '^commit' | wc -l) "1"
$OSTREE commit ${COMMIT_ARGS} -b test2-no-parent -s '' --parent=none $test_tmpdir/checkout-test2-4
assert_streq $($OSTREE log test2-no-parent |grep '^commit' | wc -l) "1"
echo "ok commit no parent"

cd ${test_tmpdir}
# Do the --bind-ref=<the other test branch>, so we store both branches sorted
# in metadata and thus the checksums remain the same.
empty_rev=$($OSTREE commit ${COMMIT_ARGS} -b test2-no-subject --bind-ref=test2-no-subject-2 -s '' --timestamp="2005-10-29 12:43:29 +0000" $test_tmpdir/checkout-test2-4)
omitted_rev=$($OSTREE commit ${COMMIT_ARGS} -b test2-no-subject-2 --bind-ref=test2-no-subject --timestamp="2005-10-29 12:43:29 +0000" $test_tmpdir/checkout-test2-4)
assert_streq $empty_rev $omitted_rev
echo "ok commit no subject"

cd ${test_tmpdir}
cat >commitmsg.txt <<EOF
This is a long
commit message.

Build-Host: foo.example.com
Crunchy-With-Extra-Ketchup: true
EOF
$OSTREE commit ${COMMIT_ARGS} -b branch-with-commitmsg -F commitmsg.txt -s 'a message' $test_tmpdir/checkout-test2-4
$OSTREE log branch-with-commitmsg > log.txt
assert_file_has_content log.txt '^ *This is a long$'
assert_file_has_content log.txt '^ *Build-Host:.*example.com$'
assert_file_has_content log.txt '^ *Crunchy-With.*true$'
$OSTREE refs --delete branch-with-commitmsg
echo "ok commit body file"

cd ${test_tmpdir}
$OSTREE commit ${COMMIT_ARGS} -b test2-custom-parent -s '' $test_tmpdir/checkout-test2-4
$OSTREE commit ${COMMIT_ARGS} -b test2-custom-parent -s '' $test_tmpdir/checkout-test2-4
$OSTREE commit ${COMMIT_ARGS} -b test2-custom-parent -s '' $test_tmpdir/checkout-test2-4
assert_streq $($OSTREE log test2-custom-parent |grep '^commit' | wc -l) "3"
prevparent=$($OSTREE rev-parse test2-custom-parent^)
$OSTREE commit ${COMMIT_ARGS} -b test2-custom-parent -s '' --parent=${prevparent} $test_tmpdir/checkout-test2-4
assert_streq $($OSTREE log test2-custom-parent |grep '^commit' | wc -l) "3"
echo "ok commit custom parent"

cd ${test_tmpdir}
orphaned_rev=$($OSTREE commit ${COMMIT_ARGS} --orphan -s "$(date)" $test_tmpdir/checkout-test2-4)
$OSTREE ls ${orphaned_rev} >/dev/null
$OSTREE prune --refs-only
if $OSTREE ls ${orphaned_rev} 2>err.txt; then
    assert_not_reached "Found orphaned commit"
fi
assert_file_has_content err.txt "No such metadata object"
echo "ok commit orphaned"

cd ${test_tmpdir}
# in bare-user-only mode, we canonicalize ownership to 0:0, so checksums won't
# match -- we could add a --ignore-ownership option I suppose?
if is_bare_user_only_repo repo; then
    echo "ok # SKIP checksums won't match up in bare-user-only"
else
    $OSTREE fsck
    CHECKSUM_FLAG=
    if [ -n "${OSTREE_NO_XATTRS:-}" ]; then
        CHECKSUM_FLAG=--ignore-xattrs
    fi
    rm -rf checksum-test
    $OSTREE checkout test2 checksum-test
    find checksum-test/ -type f | while read fn; do
        checksum=$($CMD_PREFIX ostree checksum $CHECKSUM_FLAG $fn)
        objpath=repo/objects/${checksum::2}/${checksum:2}.file
        assert_has_file $objpath
        # running `ostree checksum` on the obj might not necessarily match, let's
        # just check that they have the same content to confirm that it's
        # (probably) the originating file
        object_content_checksum=$(sha256sum $objpath | cut -f1 -d' ')
        checkout_content_checksum=$(sha256sum $fn | cut -f1 -d' ')
        assert_streq "$object_content_checksum" "$checkout_content_checksum"
    done
    echo "ok checksum CLI"
fi

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
$OSTREE diff ${DIFF_ARGS} test2 ./ > ${test_tmpdir}/diff-test2
assert_file_empty ${test_tmpdir}/diff-test2
$OSTREE diff ${DIFF_ARGS} test2 --owner-uid=$((`id -u`+1)) ./ > ${test_tmpdir}/diff-test2
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet$'
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet/message$'
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet/another/tree/green$'
echo "ok diff file with different uid"

$OSTREE diff ${DIFF_ARGS} test2 --owner-gid=$((`id -g`+1)) ./ > ${test_tmpdir}/diff-test2
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet$'
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet/message$'
assert_file_has_content ${test_tmpdir}/diff-test2 'M */yet/another/tree/green$'
echo "ok diff file with different gid"

cd ${test_tmpdir}/checkout-test2-4
rm four
mkdir four
touch four/other
$OSTREE diff test2 ./ > ${test_tmpdir}/diff-test2-2
cd ${test_tmpdir}
assert_file_has_content diff-test2-2 'M */four$'
echo "ok diff file changing type"

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    mkdir repo2
    # Use a different mode to test hardlinking metadata only
    if grep -q 'mode=archive' repo/config || is_bare_user_only_repo repo; then
        opposite_mode=bare-user
    else
        opposite_mode=archive
    fi
    ostree_repo_init repo2 --mode=$opposite_mode
    ${CMD_PREFIX} ostree --repo=repo2 pull-local repo >out.txt
    assert_file_has_content out.txt "[1-9][0-9]* metadata, [1-9][0-9]* content objects imported"
    test2_commitid=$(${CMD_PREFIX} ostree --repo=repo rev-parse test2)
    test2_commit_relpath=/objects/${test2_commitid:0:2}/${test2_commitid:2}.commit
    assert_files_hardlinked repo/${test2_commit_relpath} repo2/${test2_commit_relpath}
    echo "ok pull-local (hardlinking metadata)"
fi

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    rm repo2 -rf && mkdir repo2
    ostree_repo_init repo2 --mode=$opposite_mode
    ${CMD_PREFIX} ostree --repo=repo2 pull-local --bareuseronly-files repo test2
    ${CMD_PREFIX} ostree --repo=repo2 fsck -q
    echo "ok pull-local --bareuseronly-files"
fi

# This is mostly a copy of the suid test in test-basic-user-only.sh,
# but for the `pull --bareuseronly-files` case.
cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive
cd ${test_tmpdir}
cat > statoverride.txt <<EOF
2048 /some-setuid
EOF
mkdir -p files/
echo "a setuid file" > files/some-setuid
chmod 0644 files/some-setuid
$CMD_PREFIX ostree --repo=repo-input commit -b content-with-suid --statoverride=statoverride.txt --tree=dir=files
if $CMD_PREFIX ostree pull-local --repo=repo --bareuseronly-files repo-input content-with-suid 2>err.txt; then
    assert_not_reached "copying suid file with --bareuseronly-files worked?"
fi
assert_file_has_content err.txt 'Content object.*: invalid mode.*with bits 040.*'
echo "ok pull-local (bareuseronly files)"

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    ${CMD_PREFIX} ostree --repo=repo2 checkout ${CHECKOUT_U_ARG} test2 test2-checkout-from-local-clone
    cd test2-checkout-from-local-clone
    assert_file_has_content yet/another/tree/green 'leaf'
    echo "ok local clone checkout"
fi

$OSTREE checkout -U test2 checkout-user-test2
echo "ok user checkout"

$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Another commit" --tree=ref=test2
echo "ok commit from ref"

$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Another commit with modifier" --tree=ref=test2 --owner-uid=`id -u`
echo "ok commit from ref with modifier"

$OSTREE commit ${COMMIT_ARGS} -b trees/test2 -s 'ref with / in it' --tree=ref=test2
echo "ok commit ref with /"

mkdir badutf8
echo "invalid utf8 filename" > badutf8/$(printf '\x80')
if $OSTREE commit ${COMMIT_ARGS} -b badutf8 --tree=dir=badutf8 2>err.txt; then
    assert_not_reached "commit filename with invalid UTF-8"
fi
assert_file_has_content err.txt "Invalid UTF-8 in filename"
echo "ok commit bad UTF-8"

old_rev=$($OSTREE rev-parse test2)
$OSTREE ls -R -C test2
$OSTREE commit ${COMMIT_ARGS} --skip-if-unchanged -b trees/test2 -s 'should not be committed' --tree=ref=test2
$OSTREE ls -R -C test2
new_rev=$($OSTREE rev-parse test2)
assert_streq "${old_rev}" "${new_rev}"
echo "ok commit --skip-if-unchanged"

cd ${test_tmpdir}/checkout-test2-4
$OSTREE commit ${COMMIT_ARGS} -b test2 -s "no xattrs" --no-xattrs
echo "ok commit with no xattrs"

mkdir tree-A tree-B
touch tree-A/file-a tree-B/file-b

$OSTREE commit ${COMMIT_ARGS} -b test3-1 -s "Initial tree" --tree=dir=tree-A
$OSTREE commit ${COMMIT_ARGS} -b test3-2 -s "Replacement tree" --tree=dir=tree-B
$OSTREE commit ${COMMIT_ARGS} -b test3-combined -s "combined tree" --tree=ref=test3-1 --tree=ref=test3-2

$OSTREE checkout test3-combined checkout-test3-combined

assert_has_file checkout-test3-combined/file-a
assert_has_file checkout-test3-combined/file-b

echo "ok commit combined ref trees"

# NB: The + is optional, but we need to make sure we support it
cd ${test_tmpdir}
cat > test-statoverride.txt <<EOF
+1048 /a/nested/2
2048 /a/nested/3
=384 /a/readable-only
EOF
cd ${test_tmpdir}/checkout-test2-4
echo readable only > a/readable-only
chmod 664 a/readable-only
$OSTREE commit ${COMMIT_ARGS} -b test2-override -s "with statoverride" --statoverride=../test-statoverride.txt
cd ${test_tmpdir}
$OSTREE checkout test2-override checkout-test2-override
if ! is_bare_user_only_repo repo; then
    test -g checkout-test2-override/a/nested/2
    test -u checkout-test2-override/a/nested/3
else
    test '!' -g checkout-test2-override/a/nested/2
    test '!' -u checkout-test2-override/a/nested/3
fi
assert_file_has_mode checkout-test2-override/a/readable-only 600
echo "ok commit statoverride"

cd ${test_tmpdir}
cat > test-skiplist.txt <<EOF
/a/nested/3
EOF
cd ${test_tmpdir}/checkout-test2-4
assert_has_file a/nested/3
$OSTREE commit ${COMMIT_ARGS} -b test2-skiplist -s "with skiplist" --skip-list=../test-skiplist.txt
cd ${test_tmpdir}
$OSTREE checkout test2-skiplist checkout-test2-skiplist
assert_not_has_file checkout-test2-skiplist/a/nested/3
echo "ok commit skiplist"

cd ${test_tmpdir}
$OSTREE prune
echo "ok prune didn't fail"

cd ${test_tmpdir}
# Verify we can't cat dirs
for path in / /baz; do
    if $OSTREE cat test2 $path 2>err.txt; then
        assert_not_reached "cat directory"
    fi
    assert_file_has_content err.txt "open directory"
done
rm checkout-test2 -rf
$OSTREE cat test2 /yet/another/tree/green > greenfile-contents
assert_file_has_content greenfile-contents "leaf"
$OSTREE checkout test2 checkout-test2
ls -alR checkout-test2
ln -sr checkout-test2/{four,four-link}
ln -sr checkout-test2/{baz/cow,cow-link}
ln -sr checkout-test2/{cow-link,cow-link-link}
$OSTREE commit -b test2-withlink --tree=dir=checkout-test2
if $OSTREE cat test2-withlink /four-link 2>err.txt; then
    assert_not_reached "cat directory"
fi
assert_file_has_content err.txt "open directory"
for path in /cow-link /cow-link-link; do
    $OSTREE cat test2-withlink $path >contents.txt
    assert_file_has_content contents.txt moo
done
echo "ok cat-file"

cd ${test_tmpdir}
$OSTREE checkout --subpath /yet/another test2 checkout-test2-subpath
cd checkout-test2-subpath
assert_file_has_content tree/green "leaf"
cd ${test_tmpdir}
rm checkout-test2-subpath -rf
$OSTREE ls -R test2
# Test checking out a file
$OSTREE checkout --subpath /baz/saucer test2 checkout-test2-subpath
assert_file_has_content checkout-test2-subpath/saucer alien
# Test checking out a file without making a subdir
mkdir t
cd t
$OSTREE checkout --subpath /baz/saucer test2 .
assert_file_has_content saucer alien
rm t -rf
echo "ok checkout subpath"

cd ${test_tmpdir}
rm -rf checkout-test2-skiplist
cat > test-skiplist.txt <<EOF
/baz/saucer
/yet/another/tree
EOF
$OSTREE checkout --skip-list test-skiplist.txt test2 checkout-test2-skiplist
cd checkout-test2-skiplist
! test -f baz/saucer
! test -d yet/another/tree
test -f baz/cow
test -d baz/deeper
echo "ok checkout skip-list"

cd ${test_tmpdir}
rm -rf checkout-test2-skiplist
cat > test-skiplist.txt <<EOF
/saucer
/deeper
EOF
$OSTREE checkout --skip-list test-skiplist.txt --subpath /baz \
  test2 checkout-test2-skiplist
cd checkout-test2-skiplist
! test -f saucer
! test -d deeper
test -f cow
test -d another
echo "ok checkout skip-list with subpath"

cd ${test_tmpdir}
$OSTREE checkout  --union test2 checkout-test2-union
find checkout-test2-union | wc -l > union-files-count
$OSTREE checkout  --union test2 checkout-test2-union
find checkout-test2-union | wc -l > union-files-count.new
cmp union-files-count{,.new}
cd checkout-test2-union
assert_file_has_content ./yet/another/tree/green "leaf"
echo "ok checkout union 1"

cd ${test_tmpdir}
$OSTREE commit ${COMMIT_ARGS} -b test-union-add --tree=ref=test2
$OSTREE checkout test-union-add checkout-test-union-add
echo 'file for union add testing' > checkout-test-union-add/union-add-test
echo 'another file for union add testing' > checkout-test-union-add/union-add-test2
$OSTREE commit ${COMMIT_ARGS} -b test-union-add --tree=dir=checkout-test-union-add
rm checkout-test-union-add -rf
# Check out previous
$OSTREE checkout test-union-add^ checkout-test-union-add
assert_not_has_file checkout-test-union-add/union-add-test
assert_not_has_file checkout-test-union-add/union-add-test2
# Now create a file we don't want overwritten
echo 'existing file for union add' > checkout-test-union-add/union-add-test
$OSTREE checkout --union-add test-union-add checkout-test-union-add
assert_file_has_content checkout-test-union-add/union-add-test 'existing file for union add'
assert_file_has_content checkout-test-union-add/union-add-test2 'another file for union add testing'
echo "ok checkout union add"

# Test --union-identical <https://github.com/projectatomic/rpm-ostree/issues/982>
# Prepare data:
cd ${test_tmpdir}
for x in $(seq 3); do
    mkdir -p pkg${x}/usr/{bin,share/licenses}
    # Separate binaries and symlinks
    echo 'binary for pkg'${x} > pkg${x}/usr/bin/pkg${x}
    ln -s pkg${x} pkg${x}/usr/bin/link${x}
    # But they share the GPL
    echo 'this is the GPL' > pkg${x}/usr/share/licenses/COPYING
    ln -s COPYING pkg${x}/usr/share/licenses/LICENSE
    $OSTREE commit -b union-identical-pkg${x} --tree=dir=pkg${x}
done
rm union-identical-test -rf
for x in $(seq 3); do
    $OSTREE checkout ${CHECKOUT_H_ARGS} --union-identical union-identical-pkg${x} union-identical-test
done
if $OSTREE checkout ${CHECKOUT_H_ARGS/-H/} --union-identical union-identical-pkg${x} union-identical-test-tmp 2>err.txt; then
    fatal "--union-identical without -H"
fi
assert_file_has_content err.txt "error:.*--union-identical requires --require-hardlinks"
for x in $(seq 3); do
    for v in pkg link; do
        assert_file_has_content union-identical-test/usr/bin/${v}${x} "binary for pkg"${x}
    done
    for v in COPYING LICENSE; do
        assert_file_has_content union-identical-test/usr/share/licenses/${v} GPL
    done
done
# now checkout the first pkg in force copy mode to make sure we can checksum
rm union-identical-test -rf
$OSTREE checkout --force-copy union-identical-pkg1 union-identical-test
for x in 2 3; do
    $OSTREE checkout ${CHECKOUT_H_ARGS} --union-identical union-identical-pkg${x} union-identical-test
done
echo "ok checkout union identical merges"

# Make conflicting packages, one with regfile, one with symlink
mkdir -p pkg-conflict1bin/usr/{bin,share/licenses}
echo 'binary for pkg-conflict1bin' > pkg-conflict1bin/usr/bin/pkg1
echo 'this is the GPL' > pkg-conflict1bin/usr/share/licenses/COPYING
$OSTREE commit -b union-identical-conflictpkg1bin --tree=dir=pkg-conflict1bin
mkdir -p pkg-conflict1link/usr/{bin,share/licenses}
ln -s somewhere-else > pkg-conflict1link/usr/bin/pkg1
echo 'this is the GPL' > pkg-conflict1link/usr/share/licenses/COPYING
$OSTREE commit -b union-identical-conflictpkg1link --tree=dir=pkg-conflict1link

for v in bin link; do
    rm union-identical-test -rf
    $OSTREE checkout ${CHECKOUT_H_ARGS} --union-identical union-identical-pkg1 union-identical-test
    if $OSTREE checkout ${CHECKOUT_H_ARGS} --union-identical union-identical-conflictpkg1${v} union-identical-test 2>err.txt; then
        fatal "union identical $v succeeded?"
    fi
    assert_file_has_content err.txt 'error:.*File exists'
done
echo "ok checkout union identical conflicts"

cd ${test_tmpdir}
rm files -rf && mkdir files
mkdir files/worldwritable-dir
chmod a+w files/worldwritable-dir
$CMD_PREFIX ostree --repo=repo commit -b content-with-dir-world-writable --tree=dir=files
rm dir-co -rf
$CMD_PREFIX ostree --repo=repo checkout -U -H -M content-with-dir-world-writable dir-co
assert_file_has_mode dir-co/worldwritable-dir 775
if ! is_bare_user_only_repo repo; then
    rm dir-co -rf
    $CMD_PREFIX ostree --repo=repo checkout -U -H content-with-dir-world-writable dir-co
    assert_file_has_mode dir-co/worldwritable-dir 777
fi
rm dir-co -rf
echo "ok checkout bareuseronly dir"

cd ${test_tmpdir}
rm -rf shadow-repo
mkdir shadow-repo
ostree_repo_init shadow-repo
${CMD_PREFIX} ostree --repo=shadow-repo config set core.parent $(pwd)/repo
rm -rf test2-checkout
parent_rev_test2=$(${CMD_PREFIX} ostree --repo=repo rev-parse test2)
${CMD_PREFIX} ostree --repo=shadow-repo checkout ${CHECKOUT_U_ARG} "${parent_rev_test2}" test2-checkout
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

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    mkdir repo3
    ostree_repo_init repo3 --mode=bare-user
    ${CMD_PREFIX} ostree --repo=repo3 pull-local --remote=aremote repo test2
    ${CMD_PREFIX} ostree --repo=repo3 rev-parse aremote/test2
    echo "ok pull-local with --remote arg"
fi

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    ${CMD_PREFIX} ostree --repo=repo3 prune
    find repo3/objects -name '*.commit' > objlist-before-prune
    rm repo3/refs/heads/* repo3/refs/mirrors/* repo3/refs/remotes/* -rf
    ${CMD_PREFIX} ostree --repo=repo3 prune --refs-only
    find repo3/objects -name '*.commit' > objlist-after-prune
    if cmp -s objlist-before-prune objlist-after-prune; then
        fatal "Prune didn't delete anything!"
    fi
    rm repo3 objlist-before-prune objlist-after-prune -rf
    echo "ok prune"
fi

cd ${test_tmpdir}
rm repo3 -rf
ostree_repo_init repo3 --mode=archive
${CMD_PREFIX} ostree --repo=repo3 pull-local --remote=aremote repo test2
rm repo3/refs/remotes -rf
mkdir repo3/refs/remotes
${CMD_PREFIX} ostree --repo=repo3 prune --refs-only
find repo3/objects -name '*.filez' > file-objects
if test -s file-objects; then
    assert_not_reached "prune didn't delete all objects"
fi
echo "ok prune in archive deleted everything"

cd ${test_tmpdir}
rm -rf test2-checkout
$OSTREE checkout test2 test2-checkout
(cd test2-checkout && $OSTREE commit ${COMMIT_ARGS} --link-checkout-speedup -b test2 -s "tmp")
echo "ok commit with link speedup"

cd ${test_tmpdir}
rm -rf test2-checkout
$OSTREE checkout test2 test2-checkout
# set cow to different perms, but re-set cowro to the same perms
cat > statoverride.txt <<EOF
=$((0600)) /baz/cow
=$((0600)) /baz/cowro
EOF
$OSTREE commit ${COMMIT_ARGS} --statoverride=statoverride.txt \
  --table-output --link-checkout-speedup -b test2-tmp test2-checkout > stats.txt
$OSTREE diff test2 test2-tmp > diff-test2
assert_file_has_content diff-test2 'M */baz/cow$'
assert_not_file_has_content diff-test2 'M */baz/cowro$'
assert_not_file_has_content diff-test2 'baz/saucer'
# only /baz/cow is a cache miss
assert_file_has_content stats.txt '^Content Written: 1$'
echo "ok commit with link speedup and modifier"

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
checksum=$($OSTREE commit ${COMMIT_ARGS} -b test4 -s "Third commit")
cd ${test_tmpdir}
$OSTREE show test4 > show-output
assert_file_has_content show-output "Third commit"
assert_file_has_content show-output "commit $checksum"
echo "ok show full output"

grep -E -e '^ContentChecksum' show-output > previous-content-checksum.txt
cd $test_tmpdir/checkout-test2
checksum=$($OSTREE commit ${COMMIT_ARGS} -b test4 -s "Another commit with different subject")
cd ${test_tmpdir}
$OSTREE show test4 | grep -E -e '^ContentChecksum' > new-content-checksum.txt
if ! diff -u previous-content-checksum.txt new-content-checksum.txt; then
    fatal "content checksum differs"
fi
echo "ok content checksum"

cd $test_tmpdir/checkout-test2
checksum1=$($OSTREE commit ${COMMIT_ARGS} -b test5 -s "First commit")
checksum2=$($OSTREE commit ${COMMIT_ARGS} -b test5 -s "Second commit")
cd ${test_tmpdir}
$OSTREE log test5 > log-output
assert_file_has_content log-output "First commit"
assert_file_has_content log-output "commit $checksum1"
assert_file_has_content log-output "Second commit"
assert_file_has_content log-output "commit $checksum2"
echo "ok log output"

cd $test_tmpdir/checkout-test2
checksum1=$($OSTREE commit ${COMMIT_ARGS} -b test6 -s "First commit")
checksum2=$($OSTREE commit ${COMMIT_ARGS} -b test6 -s "Second commit")
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
$OSTREE commit ${COMMIT_ARGS} -s sometest -b test2 checkout-test2
echo "ok commit with directory filename"

cd $test_tmpdir/checkout-test2
$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Metadata string" --add-metadata-string=FOO=BAR \
        --add-metadata-string=KITTENS=CUTE --add-detached-metadata-string=SIGNATURE=HANCOCK \
        --add-metadata=SOMENUM='uint64 42' --tree=ref=test2
cd ${test_tmpdir}
$OSTREE show --print-metadata-key=FOO test2 > test2-meta
assert_file_has_content test2-meta "BAR"
$OSTREE show --print-metadata-key=KITTENS test2 > test2-meta
assert_file_has_content test2-meta "CUTE"

$OSTREE show --print-metadata-key=SOMENUM test2 > test2-meta
case "$("${test_builddir}/get-byte-order")" in
    (4321)
        assert_file_has_content test2-meta "uint64 42"
        ;;
    (1234)
        assert_file_has_content test2-meta "uint64 3026418949592973312"
        ;;
    (*)
        fatal "neither little-endian nor big-endian?"
        ;;
esac

$OSTREE show -B --print-metadata-key=SOMENUM test2 > test2-meta
assert_file_has_content test2-meta "uint64 42"
$OSTREE show --print-detached-metadata-key=SIGNATURE test2 > test2-meta
assert_file_has_content test2-meta "HANCOCK"
echo "ok metadata commit with strings"

$OSTREE commit ${COMMIT_ARGS} -b test2 --tree=ref=test2 \
   --add-detached-metadata-string=SIGNATURE=HANCOCK \
  --keep-metadata=KITTENS --keep-metadata=SOMENUM
if $OSTREE show --print-metadata-key=FOO test2; then
  assert_not_reached "FOO was kept without explicit --keep-metadata?"
fi
$OSTREE show --print-metadata-key=KITTENS test2 > test2-meta
assert_file_has_content test2-meta "CUTE"
$OSTREE show -B --print-metadata-key=SOMENUM test2 > test2-meta
assert_file_has_content test2-meta "uint64 42"
echo "ok keep metadata from parent"

cd ${test_tmpdir}
$OSTREE show --print-metadata-key=ostree.ref-binding test2 > test2-ref-binding
assert_file_has_content test2-ref-binding 'test2'

$OSTREE commit ${COMMIT_ARGS} -b test2-unbound --no-bindings --tree=dir=${test_tmpdir}/checkout-test2
if $OSTREE show --print-metadata-key=ostree.ref-binding; then
    fatal "ref bindings found with --no-bindings?"
fi
echo "ok refbinding"

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    rm repo2 -rf
    mkdir repo2
    ostree_repo_init repo2 --mode=bare-user
    ${CMD_PREFIX} ostree --repo=repo2 pull-local repo
    ${CMD_PREFIX} ostree --repo=repo2 show --print-detached-metadata-key=SIGNATURE test2 > test2-meta
    assert_file_has_content test2-meta "HANCOCK"
    echo "ok pull-local after commit metadata"
fi

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
if $OSTREE commit ${COMMIT_ARGS} -b test2 -s "Attempt to commit a FIFO" 2>../errmsg; then
    assert_not_reached "Committing a FIFO unexpetedly succeeded!"
    assert_file_has_content ../errmsg "Unsupported file type"
fi
echo "ok commit of fifo was rejected"

cd ${test_tmpdir}
rm repo2 -rf
mkdir repo2
ostree_repo_init repo2 --mode=archive
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
ls repo2/uncompressed-objects-cache > ls.txt
if ! test -s ls.txt; then
    assert_not_reached "repo didn't cache uncompressed objects"
fi
# we're in archive mode, but the repo we pull-local from might be
# bare-user-only, in which case, we skip these checks since bare-user-only
# doesn't store permission bits
if ! is_bare_user_only_repo repo; then
    assert_file_has_mode test2-checkout/baz/cowro 600
    assert_file_has_mode test2-checkout/baz/deeper/ohyeahx 755
fi
echo "ok disable cache checkout"

cd ${test_tmpdir}
rm checkout-test2 -rf
$OSTREE checkout test2 checkout-test2
date > checkout-test2/date.txt
rm repo/tmp/* -rf
export TEST_BOOTID=3072029c-8b10-60d1-d31b-8422eeff9b42
if env OSTREE_REPO_TEST_ERROR=pre-commit OSTREE_BOOTID=${TEST_BOOTID} \
       $OSTREE commit ${COMMIT_ARGS} -b test2 -s '' $test_tmpdir/checkout-test2 2>err.txt; then
    assert_not_reached "Should have hit OSTREE_REPO_TEST_ERROR_PRE_COMMIT"
fi
assert_file_has_content err.txt OSTREE_REPO_TEST_ERROR_PRE_COMMIT
found_staging=0
for d in $(find repo/tmp/ -maxdepth 1 -type d); do
    bn=$(basename $d)
    if test ${bn##staging-} != ${bn}; then
	assert_str_match "${bn}" "^staging-${TEST_BOOTID}-"
	found_staging=1
    fi
done
assert_streq "${found_staging}" 1
echo "ok test error pre commit/bootid"

# Whiteouts
cd ${test_tmpdir}
mkdir -p overlay/baz/
if touch overlay/baz/.wh.cow && touch overlay/.wh.deeper; then
    touch overlay/anewfile
    mkdir overlay/anewdir/
    touch overlay/anewdir/blah
    $OSTREE --repo=repo commit ${COMMIT_ARGS} -b overlay -s 'overlay' --tree=dir=overlay
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

    # And test replacing a directory wholesale with a symlink as well as a regular file
    mkdir overlay
    echo baz to file > overlay/baz
    ln -s anewfile overlay/anewdir
    $OSTREE --repo=repo commit ${COMMIT_ARGS} -b overlay-dir-convert --tree=dir=overlay
    rm overlay -rf

    rm overlay-co -rf
    for branch in test2 overlay-dir-convert; do
        $OSTREE --repo=repo checkout --union --whiteouts ${branch} overlay-co
    done
    assert_has_file overlay-co/baz
    test -L overlay-co/anewdir

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
else
    echo "ok # SKIP whiteouts do not work, are you using aufs?"
    echo "ok # SKIP whiteouts do not work, are you using aufs?"
fi

cd ${test_tmpdir}
rm -rf test2-checkout
mkdir -p test2-checkout
cd test2-checkout
echo 'should not be fsynced' > should-not-be-fsynced
if ! skip_one_without_strace_fault_injection; then
    # Test that --fsync=false doesn't fsync
    fsync_inject_error_ostree="strace -o /dev/null -f -e inject=syncfs,fsync,sync:error=EPERM ostree"
    ${fsync_inject_error_ostree} --repo=${test_tmpdir}/repo commit ${COMMIT_ARGS} -b test2-no-fsync --fsync=false
    # And test that we get EPERM if we inject an error
    if ${fsync_inject_error_ostree} --repo=${test_tmpdir}/repo commit ${COMMIT_ARGS} -b test2-no-fsync 2>err.txt; then
        fatal "fsync error injection failed"
    fi
    assert_file_has_content err.txt 'sync.*Operation not permitted'
    echo "ok fsync disabled"
fi

# Run this test only as non-root user.  When run as root, the chmod
# won't have any effect.
if test "$(id -u)" != "0"; then
    cd ${test_tmpdir}
    rm -f expected-fail error-message
    $OSTREE init --mode=archive --repo=repo-noperm
    chmod -w repo-noperm/objects
    $OSTREE --repo=repo-noperm pull-local repo 2> error-message || touch expected-fail
    chmod +w repo-noperm/objects
    assert_has_file expected-fail
    assert_file_has_content error-message "Permission denied"
    echo "ok unwritable repo was caught"
else
    echo "ok # SKIP not run when root"
fi

cd ${test_tmpdir}
rm -rf test2-checkout
mkdir -p test2-checkout
cd test2-checkout
touch blah
stat --printf="%.Y\n" ${test_tmpdir}/repo > ${test_tmpdir}/timestamp-orig.txt
$OSTREE commit ${COMMIT_ARGS} -b test2 -s "Should bump the mtime"
stat --printf="%.Y\n" ${test_tmpdir}/repo > ${test_tmpdir}/timestamp-new.txt
cd ..
if cmp timestamp-{orig,new}.txt; then
    assert_not_reached "failed to update mtime on repo"
fi
echo "ok mtime updated"

cd ${test_tmpdir}
$OSTREE init --mode=bare --repo=repo-extensions
assert_has_dir repo-extensions/extensions
echo "ok extensions dir"
