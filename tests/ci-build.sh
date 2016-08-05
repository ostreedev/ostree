#!/bin/bash

set -euo pipefail
set -x

NULL=
: "${ci_docker:=}"
: "${ci_parallel:=1}"
: "${ci_sudo:=no}"
: "${ci_test:=yes}"
: "${ci_test_fatal:=yes}"

if [ -n "$ci_docker" ]; then
    exec docker run \
        --env=ci_docker="" \
        --env=ci_parallel="${ci_parallel}" \
        --env=ci_sudo=yes \
        --env=ci_test="${ci_test}" \
        --env=ci_test_fatal="${ci_test_fatal}" \
        --privileged \
        ostree-ci \
        tests/ci-build.sh
fi

NOCONFIGURE=1 ./autogen.sh

srcdir="$(pwd)"
mkdir ci-build
cd ci-build

make="make -j${ci_parallel} V=1 VERBOSE=1"

../configure \
    --enable-always-build-tests \
    --enable-installed-tests \
    "$@"

${make}

maybe_fail_tests () {
    if [ "$ci_test_fatal" = yes ]; then
        exit 1
    fi
}

[ "$ci_test" = no ] || ${make} check || maybe_fail_tests
# TODO: if ostree aims to support distcheck, run that too

${make} install DESTDIR=$(pwd)/DESTDIR

( cd DESTDIR && find . )

if [ -n "$ci_sudo" ] && [ -n "$ci_test" ]; then
    sudo ${make} install
    env \
        LD_LIBRARY_PATH=/usr/local/lib \
        GI_TYPELIB_PATH=/usr/local/lib/girepository-1.0 \
        ${make} installcheck || \
    maybe_fail_tests
    env \
        LD_LIBRARY_PATH=/usr/local/lib \
        GI_TYPELIB_PATH=/usr/local/lib/girepository-1.0 \
        gnome-desktop-testing-runner -d /usr/local/share ostree/ || \
    maybe_fail_tests
fi

# vim:set sw=4 sts=4 et:
