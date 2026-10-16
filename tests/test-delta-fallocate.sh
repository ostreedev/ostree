#!/bin/bash
# SPDX-License-Identifier: LGPL-2.0+
set -euo pipefail

echo '1..4'
binary="${OSTREE_UNINSTALLED}/tests/.libs/test-repo"
if test ! -x "$binary"; then
    binary="${OSTREE_UNINSTALLED}/tests/test-repo"
fi
# Keep the sanitizer runtime ahead of the interposer in sanitizer builds.
asan=$(ldd "$binary" | awk '/libasan[.]so/ {print $3}')
for fault in unsupported enosys write sync; do
    paths="bspatch-write-multi-chunk"
    if test "$fault" = unsupported || test "$fault" = enosys; then
        paths+=" write-bspatch-multi-chunk repeated-bspatch-multi-chunk"
    fi
    for path in $paths; do
        OSTREE_TEST_FALLOCATE="$fault" \
        LD_LIBRARY_PATH="${OSTREE_UNINSTALLED}/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        LD_PRELOAD="${asan:+$asan:}${OSTREE_UNINSTALLED}/.libs/libtest-fallocate.so${LD_PRELOAD:+:$LD_PRELOAD}" \
            "$binary" -p "/repo/delta/$path" > "$fault-$path.log" 2>&1 || {
            cat "$fault-$path.log"
            exit 1
        }
    done
    echo "ok $fault"
done
