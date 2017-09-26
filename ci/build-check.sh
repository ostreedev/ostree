#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
topdir=$(git rev-parse --show-toplevel)
resultsdir=$(mktemp -d)
make check
make syntax-check  # TODO: do syntax-check under check
# See comment below
for x in test-suite.log config.log; do
    mv ${x} ${resultsdir}
done
# And now run the installed tests
make install
if test -x /usr/bin/gnome-desktop-testing-runner; then
    mkdir ${resultsdir}/gdtr-results
    # Temporary hack
    (git clone --depth=1 https://git.gnome.org/browse/gnome-desktop-testing
     cd gnome-desktop-testing
     env NOCONFIGURE=1 ./autogen.sh
     ./configure --prefix=/usr --libdir=/usr/lib64
     make && rm -f /usr/bin/ginsttest-runner && make install)
    # Use the new -L option
    gnome-desktop-testing-runner -L ${resultsdir}/gdtr-results -p 0 ${INSTALLED_TESTS_PATTERN:-libostree/}
fi

if test -x /usr/bin/clang; then
    # always fail on warnings; https://github.com/ostreedev/ostree/pull/971
    # Except for clang-4.0: error: argument unused during compilation: '-specs=/usr/lib/rpm/redhat/redhat-hardened-cc1' [-Werror,-Wunused-command-line-argument]
    export CFLAGS="-Wno-error=unused-command-line-argument -Werror ${CFLAGS:-}"
    git clean -dfx && git submodule foreach git clean -dfx
    # And now a clang build to find unused variables because it does a better
    # job than gcc for vars with cleanups; perhaps in the future these could
    # parallelize
    export CC=clang
    build
fi

# Keep this in sync with papr.yml
# TODO; Split the main/clang builds into separate build dirs
for x in test-suite.log config.log gdtr-results; do
    if test -e ${resultsdir}/${x}; then
        mv ${resultsdir}/${x} ${topdir}
    fi
done
