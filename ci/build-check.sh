#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
make check
make syntax-check  # TODO: do syntax-check under check
# And now run the installed tests
make install
if test -x /usr/bin/gnome-desktop-testing-runner; then
    gnome-desktop-testing-runner -p 0 ostree
fi

if test -x /usr/bin/clang; then
    git clean -dfx && git submodule foreach git clean -dfx
    # And now a clang build to find unused variables; perhaps
    # in the future these could parallelize
    export CC=clang
    export CFLAGS='-Werror=unused-variable'
    build
fi
