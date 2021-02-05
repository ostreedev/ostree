#!/usr/bin/bash
# Build with ASAN and UBSAN + unit tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
# We leak global state in a few places, fixing that is hard.
export ASAN_OPTIONS='detect_leaks=0'
build --disable-gtk-doc --with-curl --with-openssl --enable-sanitizers
make check
