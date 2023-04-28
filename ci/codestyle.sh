#!/usr/bin/env bash
# Tests that validate structure of the source code;
# can be run without building it.
set -eo pipefail

if command -v clang-format >/dev/null && [ -n "$GITHUB_ACTOR" ]; then
    git remote add github_actor https:///github.com/$GITHUB_ACTOR/ostree.git
    git remote add https_upstream https://github.com/ostreedev/ostree.git
    git fetch --recurse-submodules=no github_actor &
    git fetch --recurse-submodules=no https_upstream main &
    LOG_LINE=$(git show -s --format=%B | grep Merge)
    wait
    THIS_COMMIT=$(echo $LOG_LINE | awk '{print $2}')
    THAT_COMMIT=$(echo $LOG_LINE | awk '{print $4}')
    git reset --hard $THIS_COMMIT
    git rebase $THAT_COMMIT
    
    echo -n "Formatting with git clang-format... "
    if ! git clang-format $THAT_COMMIT; then
        echo "git clang-format $THAT_COMMIT failed; run: please commit the changes"
        exit 1
    fi
    echo "ok"
fi

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
