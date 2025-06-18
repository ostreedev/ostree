#!/bin/bash
# This script tests ostree-prepare-root's --soft-reboot support.

set -xeuo pipefail

# Ensure this isn't run accidentally
test "${TEST_CONTAINER}" = 1

test '!' -f /run/ostree-booted

# now we just fake out a deployment
mkdir -p /deployment/{etc,usr,sysroot}
echo newos > deployment/etc/os-release

mv /usr/lib/ostree/prepare-root.conf{,.orig}

cd /deployment
/usr/lib/ostree/ostree-prepare-root --soft-reboot

findmnt -R /run/nextroot

# Verify mountpoint setup
for d in usr; do
	mountpoint /run/nextroot/${d}
done

ls -al /run/nextroot/etc
grep -q newos /run/nextroot/etc/os-release

test -f /run/ostree-booted

mv /usr/lib/ostree/prepare-root.conf{.orig,}

echo "ok soft-reboot"
