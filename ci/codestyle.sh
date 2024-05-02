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

echo -n 'grep-based static analysis... '
patterns=(glnx_fd_close)
for pat in "${patterns[@]}"; do
    if git grep "${pat}" | grep -v codestyle\.sh; then
        echo "Files matched prohibited pattern: ${pat}" 1>&2
        exit 1
    fi
done
echo ok
