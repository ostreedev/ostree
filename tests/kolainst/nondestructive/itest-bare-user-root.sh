#!/bin/bash

# Tests of the "raw ostree" functionality using the host's ostree repo as uid 0.

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

echo "1..1"
date

prepare_tmpdir
ostree --repo=repo init --mode=bare-user
mkdir -p components/{dbus,systemd}/usr/{bin,lib}
echo dbus binary > components/dbus/usr/bin/dbus-daemon
chmod a+x components/dbus/usr/bin/dbus-daemon
echo dbus lib > components/dbus/usr/lib/libdbus.so.1
echo dbus helper > components/dbus/usr/lib/dbus-daemon-helper
chmod a+x components/dbus/usr/lib/dbus-daemon-helper
echo systemd binary > components/systemd/usr/bin/systemd
chmod a+x components/systemd/usr/bin/systemd
echo systemd lib > components/systemd/usr/lib/libsystemd.so.1

# Make the gid on dbus 81 like fedora, also ensure no xattrs
ostree --repo=repo commit --no-xattrs -b component-dbus --owner-uid 0 --owner-gid 81 --tree=dir=components/dbus
ostree --repo=repo commit --no-xattrs -b component-systemd --owner-uid 0 --owner-gid 0 --tree=dir=components/systemd
rm rootfs -rf
for component in dbus systemd; do
    ostree --repo=repo checkout -U -H component-${component} --union rootfs
done
echo 'some rootfs data' > rootfs/usr/lib/cache.txt
# Commit using the host's selinux policy
ostree --repo=repo commit --selinux-policy / -b rootfs --link-checkout-speedup --tree=dir=rootfs
ostree --repo=repo ls rootfs /usr/bin/systemd >ls.txt
assert_file_has_content ls.txt '^-007.. 0 0 .*/usr/bin/systemd'
ostree --repo=repo ls -X rootfs /usr/lib/dbus-daemon-helper >ls.txt
assert_file_has_content ls.txt '^-007.. 0 81 .*security\.selinux.*/usr/lib/dbus-daemon-helper'
assert_not_file_has_content ls.txt 'user\.ostreemeta'
echo "ok bare-user link-checkout-speedup with modified xattrs maintains uids"
date
