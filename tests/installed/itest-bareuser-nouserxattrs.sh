#!/bin/bash

# Test that initializing a bare-user repo on tmpfs fails
# Maybe at some point this will be fixed in the kernel
# but I doubt it'll be soon
# https://www.spinics.net/lists/linux-mm/msg109775.html

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

prepare_tmpdir
trap _tmpdir_cleanup EXIT

mkdir mnt
mount -t tmpfs tmpfs mnt
if ostree --repo=mnt/repo init --mode=bare-user 2>err.txt; then
    umount mnt
    assert_not_reached "bare-user on tmpfs worked?"
fi
umount mnt
assert_file_has_content err.txt "Operation not supported"
