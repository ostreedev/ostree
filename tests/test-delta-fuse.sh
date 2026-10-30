#!/bin/bash
#
# Copyright (C) 2026 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

# shellcheck source=tests/libtest.sh
# shellcheck disable=SC1091 # The test harness resolves this relative source at runtime.
. "$(dirname "$0")/libtest.sh"

# Exercise automatic FUSE detection rather than the test-only mmap error hook.
unset OSTREE_REPO_TEST_ERROR

run_ostree() {
    # CMD_PREFIX is a test-harness command prefix and intentionally splits.
    # shellcheck disable=SC2086
    ${CMD_PREFIX} ostree "$@"
}

skip_without_fuse
skip_without_user_xattrs

echo "1..1"

# shellcheck disable=SC2154 # libtest.sh creates test_tmpdir.
cd "${test_tmpdir}"
mkdir source source-files final staging-backing staging-mnt
ostree_repo_init source --mode=archive

# Use fixed, incompressible input so the delta contains a BSPATCH operation.
# The target retains its final quarter, which makes BSPATCH preferable to a
# rollsum delta without depending on host binaries or random input.
python3 - source-files/direct-io-content expected-target <<'EOF'
import hashlib
import sys

def bytes_for(label, size):
    result = bytearray()
    counter = 0
    while len(result) < size:
        result.extend(hashlib.sha256(label + counter.to_bytes(4, 'big')).digest())
        counter += 1
    return result[:size]

base = bytes_for(b'base', 256 * 1024)
target = bytearray(base)
target[:192 * 1024] = bytes_for(b'target', 192 * 1024)
target.extend(b'changed-by-static-delta\n')
open(sys.argv[1], 'wb').write(base)
open(sys.argv[2], 'wb').write(target)
EOF
run_ostree --repo=source commit --owner-uid=0 --owner-gid=0 --no-xattrs \
    --canonical-permissions -b main -s base --tree=dir=source-files
base_rev=$(run_ostree --repo=source rev-parse main)
cp expected-target source-files/direct-io-content
run_ostree --repo=source commit --owner-uid=0 --owner-gid=0 --no-xattrs \
    --canonical-permissions -b main -s target --tree=dir=source-files
target_rev=$(run_ostree --repo=source rev-parse main)
run_ostree --repo=source static-delta generate --from="${base_rev}" --to="${target_rev}" \
    --min-fallback-size=0 --max-bsdiff-size=2 2>&1 | tee delta-generate.txt
assert_file_has_content delta-generate.txt 'bsdiff=[1-9][0-9]* objects'
run_ostree --repo=source static-delta show "${base_rev}-${target_rev}" > delta-show.txt
assert_file_has_content delta-show.txt 'Number of parts: [1-9][0-9]*'
run_ostree --repo=source summary -u

# The final repository supplies the base through core.parent; the temporary
# child repository itself starts empty, as it does for Flatpak's system helper.
ostree_repo_init final --mode=bare-user-only
run_ostree --repo=final pull-local --untrusted --disable-static-deltas source "${base_rev}"

mkdir httpd
ln -s "${test_tmpdir}/source" httpd/source
pushd httpd
run_webserver
popd

rofiles-fuse --copyup staging-backing staging-mnt -o direct_io
staging_fuse_mounted=true
cleanup_staging_fuse() {
    if test "${staging_fuse_mounted}" = true; then
        fusermount -u "${test_tmpdir}/staging-mnt" || true
    fi
}
libtest_exit_cmds+=(cleanup_staging_fuse)

# direct_io must reject the writable shared mappings normally used by BSPATCH.
touch staging-mnt/mmap-probe
python3 - staging-mnt/mmap-probe <<'EOF'
import errno
import mmap
import os
import sys

fd = os.open(sys.argv[1], os.O_RDWR)
try:
    os.ftruncate(fd, 4096)
    try:
        mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_WRITE)
    except OSError as error:
        if error.errno != errno.ENODEV:
            raise
    else:
        raise RuntimeError('shared writable mmap unexpectedly succeeded')
finally:
    os.close(fd)
EOF
rm staging-mnt/mmap-probe

ostree_repo_init staging-mnt --mode=bare-user-only
run_ostree --repo=staging-mnt config set core.parent "${test_tmpdir}/final"
httpd_address=$(<httpd-address)
run_ostree --repo=staging-mnt remote add --set=gpg-verify=false origin "${httpd_address}/source"

# Make a loose-content fallback impossible.  The commit object remains
# available for remote ref resolution, while the static delta carries the file.
target_object=$(ostree_file_path_to_object_path source "${target_rev}" direct-io-content)
rm "${target_object}"
run_ostree --repo=staging-mnt pull --require-static-deltas origin main | tee delta-pull.txt
assert_file_has_content delta-pull.txt '[1-9][0-9]* delta parts'
assert_file_has_content delta-pull.txt '0 bytes content written'

fusermount -u staging-mnt
staging_fuse_mounted=false
run_ostree --repo=final pull-local --untrusted staging-backing "${target_rev}"
assert_ref final "${target_rev}" "${target_rev}"
run_ostree --repo=final fsck
run_ostree --repo=final checkout "${target_rev}" final-checkout
cmp expected-target final-checkout/direct-io-content

echo "ok static delta direct I/O FUSE staging import"
