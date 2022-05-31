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

# First, basic static analysis
./ci/codestyle.sh

NOCONFIGURE=1 ./autogen.sh

srcdir="$(pwd)"
mkdir ci-build
cd ci-build

# V=1 shows the full build commands. VERBOSE=1 dumps test-suite.log on
# failures.
make="make V=1 VERBOSE=1"

../configure \
    --enable-always-build-tests \
    "$@"

${make}

# Run the tests both using check and distcheck.
${make} check

# Some tests historically failed when package builds set this.
# By setting it for distcheck but not check, we exercise both ways.
export SOURCE_DATE_EPOCH=$(date '+%s')

${make} distcheck DISTCHECK_CONFIGURE_FLAGS="$*"

# Show the installed files
${make} install DESTDIR=$(pwd)/DESTDIR
( cd DESTDIR && find . )

# vim:set sw=4 sts=4 et:
