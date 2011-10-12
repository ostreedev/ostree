#!/bin/bash

set -e

. libtest.sh

echo '1..5'

mkdir files
files=`pwd`/files
touch files/foo
echo moo > files/cow

mkdir repo
cd repo
hacktree init
echo 'ok init'
hacktree fsck -q
echo 'ok fsck'
hacktree link-file $files/foo
echo 'ok link'
hacktree fsck -q
echo 'ok link-fsk'
hacktree link-file $files/cow
hacktree fsck -q
echo 'ok link-fsk2'

