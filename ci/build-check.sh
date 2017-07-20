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
    gnome-desktop-testing-runner -p 0 ${INSTALLED_TESTS_PATTERN:-libostree/}
fi

if test -x /usr/bin/clang; then
    # always fail on warnings; https://github.com/ostreedev/ostree/pull/971
    # Except for clang-4.0: error: argument unused during compilation: '-specs=/usr/lib/rpm/redhat/redhat-hardened-cc1' [-Werror,-Wunused-command-line-argument]
    export CFLAGS="Wno-error=unused-command-line-argument -Werror ${CFLAGS:-}"
    git clean -dfx && git submodule foreach git clean -dfx
    # And now a clang build to find unused variables because it does a better
    # job than gcc for vars with cleanups; perhaps in the future these could
    # parallelize
    export CC=clang
    build
fi
