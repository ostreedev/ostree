#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
make check
make install
git clean -dfx

# And now a clang build to find unused variables; perhaps
# in the future these could parallelize
export CC=clang
export CFLAGS='-Werror=unused-variable'
build_default

# And now run the installed tests
gnome-desktop-testing-runner -p 0 ostree
