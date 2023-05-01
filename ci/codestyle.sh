#!/usr/bin/env bash
# Tests that validate structure of the source code;
# can be run without building it.
set -euo pipefail

# Don't hard require Rust
if command -v cargo >/dev/null; then
echo -n "checking rustfmt... "
for crate in $(find -iname Cargo.toml); do
    if ! cargo fmt --manifest-path ${crate} -- --check; then
        echo "cargo fmt failed; run: cd $(dirname ${crate}) && cargo fmt" 1>&2
        exit 1
    fi
done
echo "ok"
fi

if command -v clang-format; then
    clang_ver=$(clang-format --version)
    clang_min_ver=15
    version_re=" version ([0-9]+)."
    if [[ $clang_ver =~ $version_re ]]; then
        if test "${BASH_REMATCH[1]}" -ge "${clang_min_ver}"; then
            echo -n "checking clang-format... "
            git ls-files '**.c' '**.cxx' '**.h' '**.hpp' | xargs clang-format --Werror --dry-run
            echo "ok"
        else
            echo "notice: clang-format ${clang_ver}" is too old
        fi
    else
        echo "failed to parse clang-format version ${clang_ver}" 1>&2
        exit 1
    fi
fi

echo -n 'grep-based static analysis... '
patterns=(glnx_fd_close)
for pat in "${patterns[@]}"; do
    if git grep "${pat}" | grep -v codestyle\.sh; then
        echo "Files matched prohibited pattern: ${pat}" 1>&2
        exit 1
    fi
done
echo ok
