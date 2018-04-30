#!/bin/bash

# Run test-basic.sh as root.
# https://github.com/ostreedev/ostree/pull/1199

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/../libinsttest.sh

date
# These tests sort of bypass the installed-tests spec;
# fixing that would require installing g-d-t-r, though
# more ideally we architect things with a "control" container
# distinct from the host.
export G_TEST_SRCDIR=$(realpath $dn/../../..)

# Use /var/tmp to hopefully use XFS + O_TMPFILE etc.
prepare_tmpdir /var/tmp
trap _tmpdir_cleanup EXIT
/usr/libexec/installed-tests/libostree/test-basic.sh
/usr/libexec/installed-tests/libostree/test-basic-c
date

# Test error message when opening a non-world-readable object
# https://github.com/ostreedev/ostree/issues/1562
rm repo -rf
chmod a+rx .
ostree --repo=repo init --mode=bare
mkdir files
touch files/unreadable
chmod 0 files/unreadable
ostree --repo=repo commit -b testbranch --tree=dir=files
# We should be able to read as non-root due to CAP_DAC_OVERRIDE
ostree --repo=repo ls testbranch >/dev/null
cat >upriv.sh <<EOF
#!/bin/bash
set -xeuo pipefail
ostree --repo=testclone
EOF
if setpriv --reuid bin --regid bin --clear-groups ostree --repo=repo cat testbranch /unreadable 2>err.txt; then
    fatal "Listed unreadable object as non-root"
fi
assert_file_has_content err.txt "Opening content object.*openat: Permission denied"
