#!/bin/bash

# Verify our /etc merge works with selinux

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot

date
# Create a new deployment
ostree admin deploy --karg-proc-cmdline ${host_commit}
new_deployment_path=/ostree/deploy/${host_osname}/deploy/${host_commit}.1

# Test /etc directory mtime
if ! test ${new_deployment_path}/etc/NetworkManager -nt /etc/NetworkManager; then
    ls -al ${new_deployment_path}/etc/NetworkManager /etc/NetworkManager
    fatal "/etc directory mtime not newer"
fi

# A set of files that have a variety of security contexts
for file in fstab passwd exports hostname sysctl.conf yum.repos.d \
            NetworkManager/dispatcher.d/hook-network-manager; do
    if ! test -e /etc/${file}; then
        continue
    fi

    current=$(cd /etc && ls -Z ${file})
    new=$(cd ${new_deployment_path}/etc && ls -Z ${file})
    assert_streq "${current}" "${new}"
done

# Cleanup
ostree admin undeploy 0

cd /ostree/repo/tmp
ostree checkout --fsync=0 -H ${host_commit} test-label
rm -vf test-label/usr/lib/ostree-boot/vmlinuz*
rm -vf test-label/usr/lib/ostree-boot/initramfs*
cd test-label/usr/lib/modules/*
rm initramfs.img
echo new initramfs > initramfs.img
cd -
ostree commit --link-checkout-speedup --selinux-policy=test-label -b test-label --consume --tree=dir=test-label

ostree admin deploy --karg-proc-cmdline test-label

# This captures all of the boot entries; it'd be slightly annoying
# to try to figure out the accurate one, so let's just ensure that at least
# one entry is boot_t.
# https://bugzilla.redhat.com/show_bug.cgi?id=1536991
ls -Z /boot/ostree/*/ > bootlsz.txt
assert_file_has_content_literal bootlsz.txt 'system_u:object_r:boot_t:s0 vmlinuz-'
assert_file_has_content_literal bootlsz.txt 'system_u:object_r:boot_t:s0 initramfs-'

ostree admin undeploy 0
ostree refs --delete test-label
date
