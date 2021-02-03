#!/usr/bin/bash
# Build with ASAN and UBSAN + unit tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
export CFLAGS='-fsanitize=address -fsanitize=undefined -fsanitize-undefined-trap-on-error'
# We leak global state in a few places, fixing that is hard.
export ASAN_OPTIONS='detect_leaks=0'
${dn}/build.sh
make check
