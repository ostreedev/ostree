#!/bin/bash
# Test: Verify ostree system state on a booted image-mode system.
#
# Expects to run after provision-packit.sh + reboot has converted the
# VM to image mode. The TMT prepare phase handles that.
#
# Translated from tests/bootc-integration/src/tests/privileged.rs
# (booted_test! tests from commit f351a7ae).
set -xeuo pipefail

echo "=== verify_ostree_booted ==="
test -f /run/ostree-booted
status=$(ostree admin status)
test -n "$status"

echo "=== verify_sysroot ==="
test -d /sysroot
test -d /ostree
test -d /ostree/repo
test -d /ostree/deploy
# Verify there's at least one stateroot
found_stateroot=0
for d in /ostree/deploy/*/; do
    if [ -d "$d" ]; then
        found_stateroot=1
        break
    fi
done
test "$found_stateroot" -eq 1

echo "=== verify_composefs ==="
fstype=$(findmnt -n -o FSTYPE /)
if [ "$fstype" = "overlay" ]; then
    # composefs is active — verify private dir
    test -e /run/ostree/.private
    mode=$(stat -c '%a' /run/ostree/.private)
    test "$mode" = "0"
else
    echo "Root filesystem is $fstype, not overlay — composefs not active, skipping"
fi

echo "=== verify_ostree_cli ==="
ostree --version | grep -q libostree
ostree admin status | grep -q .
ostree refs --repo=/ostree/repo

echo "=== verify_sysroot_readonly ==="
options=$(findmnt -n -o OPTIONS /sysroot)
echo "$options" | grep -q "ro"

echo "=== verify_ostree_run_metadata ==="
mode=$(stat -c '%a' /run/ostree)
test "$mode" = "755"

echo "=== verify_immutable_bit ==="
fstype=$(findmnt -n -o FSTYPE /)
if [ "$fstype" = "overlay" ]; then
    echo "composefs active, immutable bit check not applicable — skipping"
else
    lsattr -d / | grep -q "\-i\-"
fi

echo "=== verify_osinit_unshare ==="
ostree admin os-init ostreetestsuite
mode=$(stat -c '%a' /run/ostree)
test "$mode" = "755"

echo "=== verify_selinux_labels ==="
ls_output=$(ls -dZ /etc)
if echo "$ls_output" | grep -q ':'; then
    echo "$ls_output" | grep -q ':etc_t:'
else
    echo "SELinux appears disabled, skipping label check"
fi

echo "All booted verification tests passed."
