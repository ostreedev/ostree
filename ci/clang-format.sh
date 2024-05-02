#!/usr/bin/env bash
# Tests that validate structure of the source code;
# can be run without building it.
set -euo pipefail

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
