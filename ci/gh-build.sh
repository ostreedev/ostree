#!/bin/bash

# Copyright © 2015-2016 Collabora Ltd.
# Copyright © 2021 Endless OS Foundation LLC
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

NOCONFIGURE=1 ./autogen.sh

srcdir="$(pwd)"
mkdir ci-build
cd ci-build

make="make V=1 VERBOSE=1"

../configure \
    --enable-always-build-tests \
    "$@"

${make}

# Run the tests both using check and distcheck and dump the logs on
# failures. For distcheck the logs will be inside the dist directory, so
# tell make to use the current directory.
if ! ${make} check; then
    cat test-suite.log || :
    exit 1
fi
if ! ${make} distcheck \
    TEST_SUITE_LOG=$(pwd)/test-suite.log \
    DISTCHECK_CONFIGURE_FLAGS="$*"
then
    cat test-suite.log || :
    exit 1
fi

# Show the installed files
${make} install DESTDIR=$(pwd)/DESTDIR
( cd DESTDIR && find . )

# vim:set sw=4 sts=4 et:
