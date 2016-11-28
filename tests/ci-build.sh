#!/bin/bash

# Copyright © 2015-2016 Collabora Ltd.
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

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
        ci-image \
        tests/ci-build.sh
fi

maybe_fail_tests () {
    if [ "$ci_test_fatal" = yes ]; then
        exit 1
    fi
}

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
[ "$ci_test" = no ] || ${make} check || maybe_fail_tests
cat test/test-suite.log || :
[ "$ci_test" = no ] || ${make} distcheck || maybe_fail_tests
cat test/test-suite.log || :

${make} install DESTDIR=$(pwd)/DESTDIR
( cd DESTDIR && find . )

if [ "$ci_sudo" = yes ] && [ "$ci_test" = yes ]; then
    sudo ${make} install
    env \
        LD_LIBRARY_PATH=/usr/local/lib \
        GI_TYPELIB_PATH=/usr/local/lib/girepository-1.0 \
        ${make} installcheck || \
    maybe_fail_tests
    cat test/test-suite.log || :

    env \
        LD_LIBRARY_PATH=/usr/local/lib \
        GI_TYPELIB_PATH=/usr/local/lib/girepository-1.0 \
        gnome-desktop-testing-runner -d /usr/local/share ostree/ || \
    maybe_fail_tests
fi

# vim:set sw=4 sts=4 et:
