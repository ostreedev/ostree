#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
topdir=$(git rev-parse --show-toplevel)
resultsdir=$(mktemp -d)
make check
# See comment below
for x in test-suite.log config.log; do
    mv ${x} ${resultsdir}
done
# And now install; we'll run the test suite after we do a clang build first
# (But we don't install that one)
make install

# And now a clang build to find unused variables because it does a better
# job than gcc for vars with cleanups; perhaps in the future these could
# parallelize
if test -x /usr/bin/clang; then
    if grep -q -e 'static inline.*_GLIB_AUTOPTR_LIST_FUNC_NAME' /usr/include/glib-2.0/glib/gmacros.h; then
        echo 'Skipping clang check, see https://bugzilla.gnome.org/show_bug.cgi?id=796346'
    else
    export CFLAGS="-Wall -Werror -Wno-error=deprecated-declarations ${CFLAGS:-}"
    git clean -dfx && git submodule foreach git clean -dfx
    export CC=clang
    build
    fi
fi

copy_out_gdtr_artifacts() {
    # Keep this in sync with papr.yml
    # TODO; Split the main/clang builds into separate build dirs
    local artifactdir
    artifactdir=${ARTIFACTS:-${topdir}}
    mkdir -p "${artifactdir}"
    for x in test-suite.log config.log gdtr-results; do
        if test -e ${resultsdir}/${x}; then
            mv ${resultsdir}/${x} ${artifactdir}
        fi
    done
}

if test -x /usr/bin/gnome-desktop-testing-runner; then
    mkdir ${resultsdir}/gdtr-results
    # Temporary hack
    (git clone --depth=1 https://git.gnome.org/browse/gnome-desktop-testing
     cd gnome-desktop-testing
     env NOCONFIGURE=1 ./autogen.sh
     ./configure --prefix=/usr --libdir=/usr/lib64
     make && rm -f /usr/bin/ginsttest-runner && make install)
    # set a trap in case a test fails
    trap copy_out_gdtr_artifacts EXIT
    # Use the new -L option
    gnome-desktop-testing-runner -L ${resultsdir}/gdtr-results -p 0 ${INSTALLED_TESTS_PATTERN:-libostree/}
fi
