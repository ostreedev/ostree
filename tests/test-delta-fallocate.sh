#!/bin/bash
# SPDX-License-Identifier: LGPL-2.0+
set -euo pipefail

. $(dirname $0)/libtest.sh

if ! have_strace_fault_injection; then
    skip "this test requires strace fault injection"
fi

echo '1..8'
binary="${OSTREE_UNINSTALLED}/tests/.libs/test-repo"
if test ! -x "$binary"; then
    binary="${OSTREE_UNINSTALLED}/tests/test-repo"
fi
export LD_LIBRARY_PATH="${OSTREE_UNINSTALLED}/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# LeakSanitizer cannot run under ptrace. The ordinary test-repo run still checks leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0"

# Find the patch allocation in a real trace instead of depending on the number
# of setup allocations. The fixture's patch is 65537 bytes; its reference
# object is 65538 bytes. Confirm the injected call below still targets the patch.
declare -A patch_calls patch_writes
for path in bspatch-write-multi-chunk write-bspatch-multi-chunk repeated-bspatch-multi-chunk; do
    strace -f -o "$path.trace" -e trace=fallocate,pwrite64 \
        "$binary" -p "/repo/delta/$path" > "$path.log" 2>&1 || {
        cat "$path.log"
        exit 1
    }
    patch_calls[$path]=$(awk '/fallocate\(/ { n++; if ($0 ~ /, 65537\)/) { print n; exit } }' "$path.trace")
    patch_writes[$path]=$(awk '/pwrite64\(/ { n++ } /fallocate\(.*, 65537\)/ { print n + 1; exit }' "$path.trace")
    test -n "${patch_calls[$path]}" || fatal "patch allocation missing from $path.trace"
done

for fault in success eintr unsupported enosys enospc eio fallback-enospc fallback-eio; do
    if [[ "$fault" = unsupported || "$fault" = fallback-* ]] && ! getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
        echo "ok $fault # SKIP requires glibc's posix_fallocate emulation"
        continue
    fi
    paths="bspatch-write-multi-chunk"
    if test "$fault" = success || test "$fault" = eintr || test "$fault" = unsupported; then
        paths+=" write-bspatch-multi-chunk repeated-bspatch-multi-chunk"
    fi
    for path in $paths; do
        inject=()
        expected_error=""
        case "$fault" in
            success) ;;
            eintr) inject=(-e "inject=fallocate:error=EINTR:when=${patch_calls[$path]}") ;;
            unsupported|fallback-*) inject=(-e "inject=fallocate:error=EOPNOTSUPP:when=${patch_calls[$path]}") ;;
            *)
                expected_error="$fault"
                inject=(-e "inject=fallocate:error=${fault^^}:when=${patch_calls[$path]}")
                ;;
        esac
        if [[ "$fault" = fallback-* ]]; then
            expected_error="${fault#fallback-}"
            inject+=(-e "inject=pwrite64:error=${expected_error^^}:when=${patch_writes[$path]}")
        fi
        OSTREE_TEST_FALLOCATE_ERROR="$expected_error" \
        strace -f -o "$fault-$path.trace" -e trace=fallocate,pwrite64 "${inject[@]}" \
            "$binary" -p "/repo/delta/$path" > "$fault-$path.log" 2>&1 || {
            cat "$fault-$path.log"
            cat "$fault-$path.trace"
            exit 1
        }
        if test "$fault" != success; then
            assert_file_has_content "$fault-$path.trace" 'fallocate(.*, 65537).*INJECTED'
        fi
        if test "$fault" = unsupported; then
            assert_file_has_content "$fault-$path.trace" 'pwrite64('
        elif [[ "$fault" = fallback-* ]]; then
            assert_file_has_content "$fault-$path.trace" 'pwrite64(.*INJECTED'
        fi
    done
    echo "ok $fault"
done
