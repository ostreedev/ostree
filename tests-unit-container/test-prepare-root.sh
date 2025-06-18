#!/bin/bash
# This script tests ostree-prepare-root.service. It expects to run in
# a podman container. See the `privunit` job in Justfile.
# Here we're treating the podman container like an initramfs.

set -xeuo pipefail

# Ensure this isn't run accidentally
test "${TEST_CONTAINER}" = 1

cleanup() {
	if mountpoint /target-sysroot &>/dev/null; then
		umount -lR /target-sysroot
	fi
	rm -rf /run/ostree-booted /run/ostree
}
trap cleanup EXIT

test '!' -f /run/ostree-booted

mkdir /target-sysroot
# Needs to be a mount point
mount --bind /target-sysroot /target-sysroot
ostree admin init-fs --epoch=1 /target-sysroot
cd /target-sysroot
ostree admin --sysroot=. stateroot-init default
# now we just fake out a deployment
mkdir -p ostree/deploy/default/deploy/1234/{etc,usr,sysroot}

ln -sr ostree/deploy/default/deploy/1234 boot/ostree.0
t=$(mktemp)
# Need to disable composefs in an unprivileged container
echo "root=UUID=cafebabe ostree.prepare-root.composefs=0 ostree=/boot/ostree.0" > ${t}
mount --bind $t /proc/cmdline

cd /
/usr/lib/ostree/ostree-prepare-root /target-sysroot

findmnt -R /target-sysroot

# Verify we have this stamp file
test -f /run/ostree-booted

# Note that usr is a bind mount in legacy mode without compsoefs
for d in etc usr; do
	mountpoint /target-sysroot/${d}
done

# Default is ro in our images
grep -q 'readonly.*true' /usr/lib/ostree/prepare-root.conf
[[ "$(findmnt -n -o OPTIONS /target-sysroot/sysroot)" == *ro* ]]

cleanup
test '!' -f /run/ostree-booted

mv /usr/lib/ostree/prepare-root.conf{,.orig}

mount --bind /target-sysroot /target-sysroot
/usr/lib/ostree/ostree-prepare-root /target-sysroot
findmnt -R /target-sysroot
[[ "$(findmnt -n -o OPTIONS /target-sysroot/sysroot)" == *rw* ]]

mv /usr/lib/ostree/prepare-root.conf{.orig,}

echo "ok verified default prepare-root"
