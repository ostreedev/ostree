#!/bin/bash
# This script tests ostree-prepare-root.service. It expects to run in
# a podman container. See the `privunit` job in Justfile.
# Here we're treating the podman container like an initramfs.

set -xeuo pipefail

# Ensure this isn't run accidentally
test "${TEST_CONTAINER}" = 1

cleanup() {
	for mnt in /target-sysroot /sysroot.tmp; do
		if mountpoint "$mnt" &>/dev/null; then
			umount -lR "$mnt"
		fi
	done
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
mkdir -p ostree/deploy/default/deploy/1234/{boot,etc,usr/etc,usr/bin,sysroot}
# Populate some data
(cd ostree/deploy/default/deploy/1234
 echo passwd > usr/etc/passwd
 echo bash > usr/bin/bash
)

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
# etc is not transient by default
etc_options=$(findmnt -no OPTIONS /target-sysroot/etc)
[[ ! $etc_options =~ "upperdir=/run/ostree/transient-etc" ]]
# We don't have /boot as a bind mount by default here
if mountpoint /target-sysroot/boot &>/dev/null; then
	exit 1
fi

# Default is ro in our images
grep -q 'readonly.*true' /usr/lib/ostree/prepare-root.conf
[[ "$(findmnt -n -o OPTIONS /target-sysroot/sysroot)" == *ro* ]]

cleanup
test '!' -f /run/ostree-booted

# Test with the default config
mv /usr/lib/ostree/prepare-root.conf{,.orig}

mount --bind /target-sysroot /target-sysroot
/usr/lib/ostree/ostree-prepare-root /target-sysroot
findmnt -R /target-sysroot
[[ "$(findmnt -n -o OPTIONS /target-sysroot/sysroot)" == *rw* ]]

# Reset the config to what's in the image
mv /usr/lib/ostree/prepare-root.conf{.orig,}

cleanup

echo "ok verified default prepare-root"

cp /usr/lib/ostree/prepare-root.conf{,.orig}
cat <<EOF >>/usr/lib/ostree/prepare-root.conf
[etc]
transient = true
EOF

mount --bind /target-sysroot /target-sysroot
/usr/lib/ostree/ostree-prepare-root /target-sysroot

# Verify we have a tmpfs upper for etc
etc_options=$(findmnt -no OPTIONS /target-sysroot/etc)
[[ $etc_options =~ "upperdir=/run/ostree/transient-etc" ]]

# Reset the config
mv /usr/lib/ostree/prepare-root.conf{.orig,}
cleanup

echo "ok verified etc.transient"

# Set up boot/loader via traditional ostree swapped symlink pattern
# which will cause prepare-root to also make a bind mount.
mkdir /target-sysroot/boot/loader.0
ln -s /target-sysroot/boot/loader.0 /target-sysroot/boot/loader 

mount --bind /target-sysroot /target-sysroot
/usr/lib/ostree/ostree-prepare-root /target-sysroot

mountpoint /target-sysroot/boot

cleanup

echo "ok verified /boot"
