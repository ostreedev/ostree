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

echo "1..29"

. libtest.sh

setup_test_repository "regular"
echo "ok setup"

$OSTREE checkout test2 checkout-test2
echo "ok checkout"

$OSTREE rev-parse test2
$OSTREE rev-parse 'test2^'
$OSTREE rev-parse 'test2^^' 2>/dev/null && (echo 1>&2 "rev-parse test2^^ unexpectedly succeeded!"; exit 1)
echo "ok rev-parse"

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
assert_file_has_content diff-test2-2 'A */oh-look-a-file$'
echo "ok diff cwd"

cd ${test_tmpdir}/checkout-test2-4
rm four
mkdir four
touch four/other
$OSTREE diff test2 ./ > ${test_tmpdir}/diff-test2-2
cd ${test_tmpdir}
assert_file_has_content diff-test2-2 'M */four$'
echo "ok diff file changing type"

cd ${test_tmpdir}/checkout-test2-4
echo afile > oh-look-a-file
cat > ${test_tmpdir}/ostree-commit-metadata <<EOF
{'origin': <'http://example.com'>, 'buildid': <@u 42>}
EOF
$OSTREE commit -b test2 -s "Metadata test" --metadata-variant-text=${test_tmpdir}/ostree-commit-metadata
echo "ok metadata commit"

cd ${test_tmpdir}
rm ostree-commit-metadata
$OSTREE show test2 > ${test_tmpdir}/show
assert_file_has_content ${test_tmpdir}/show 'example.com'
assert_file_has_content ${test_tmpdir}/show 'buildid'
echo "ok metadata content"

cd ${test_tmpdir}
mkdir repo2
ostree --repo=repo2 init
$OSTREE local-clone repo2
echo "ok local clone"

cd ${test_tmpdir}
ostree --repo=repo2 checkout test2 test2-checkout-from-local-clone
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

$OSTREE commit -b test2 -s "Metadata string" --add-metadata-string=FOO=BAR --add-metadata-string=KITTENS=CUTE --tree=ref=test2
$OSTREE show test2 > test2-commit-text
assert_file_has_content test2-commit-text "'FOO'.*'BAR'"
assert_file_has_content test2-commit-text "'KITTENS'.*'CUTE'"
echo "ok metadata commit with strings"

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
$OSTREE checkout --atomic-retarget test2 checkout-atomic-test2
cd checkout-atomic-test2
assert_file_has_content ./yet/another/tree/green "leaf"
echo "ok checkout atomic"

cd ${test_tmpdir}
rm -rf test2
$OSTREE checkout --atomic-retarget test2
cd test2
assert_file_has_content ./yet/another/tree/green "leaf"
echo "ok checkout short form"
