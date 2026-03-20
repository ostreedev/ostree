#!/bin/bash
# Test: Privileged ostree operations that only need root + ostree binary.
#
# Translated from tests/bootc-integration/src/tests/privileged.rs
# (privileged_test! tests from commit f351a7ae).
set -xeuo pipefail

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

echo "=== verify_nofifo ==="
cd "$tmpdir"
mkdir -p nofifo-repo nofifo-root
ostree --repo=nofifo-repo init --mode=archive
mkfifo nofifo-root/afile
if ostree --repo=nofifo-repo commit -b fifotest -s 'commit fifo' --tree=dir=./nofifo-root 2>nofifo-err.txt; then
    echo "ERROR: committing a FIFO should have failed" >&2
    exit 1
fi
grep -q "Not a regular file or symlink" nofifo-err.txt

echo "=== verify_mtime ==="
cd "$tmpdir"
mkdir -p mtime-repo mtime-root
ostree --repo=mtime-repo init --mode=archive
echo afile > mtime-root/afile
ostree --repo=mtime-repo commit -b test --tree=dir=mtime-root > /dev/null
ts_before=$(stat -c '%Y' mtime-repo)
sleep 1
ostree --repo=mtime-repo commit -b test -s 'bump mtime' --tree=dir=mtime-root > /dev/null
ts_after=$(stat -c '%Y' mtime-repo)
test "$ts_before" != "$ts_after"

echo "=== verify_extensions ==="
cd "$tmpdir"
mkdir -p ext-repo
ostree --repo=ext-repo init --mode=bare
test -d ext-repo/extensions

echo "All privileged tests passed."
