#!/bin/bash

# Run test-basic.sh as root.
# https://github.com/ostreedev/ostree/pull/1199

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

# These tests sort of bypass the installed-tests spec;
# fixing that would require installing g-d-t-r, though
# more ideally we architect things with a "control" container
# distinct from the host.
export G_TEST_SRCDIR=$(realpath $dn/../..)

# Use /var/tmp to hopefully use XFS + O_TMPFILE etc.
tempdir=$(mktemp -d /var/tmp/tap-test.XXXXXX)
touch ${tempdir}/.testtmp
function cleanup () {
    if test -f ${tempdir}/.testtmp; then
	      rm "${tempdir}" -rf
    fi
}
trap cleanup EXIT
cd ${tempdir}
/usr/libexec/installed-tests/libostree/test-basic.sh
/usr/libexec/installed-tests/libostree/test-basic-c
