#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

prepare_tmpdir

echo "testing boot=${AUTOPKGTEST_REBOOT_MARK:-}"

# Print this by default on each boot
ostree admin status

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # xref https://github.com/coreos/coreos-assembler/pull/2814
  systemctl mask --now zincati

  test '!' -w /

  cp /usr/lib/ostree/prepare-root.conf /etc/ostree/
  cat >> /etc/ostree/prepare-root.conf <<'EOF'
[root]
transient-ro = true
EOF

  rpm-ostree initramfs-etc --track /etc/ostree/prepare-root.conf
  
  /tmp/autopkgtest-reboot "2"
  ;;
  "2")

  test '!' -w '/'

  unshare -m /bin/sh -c 'env LIBMOUNT_FORCE_MOUNT2=always mount -o remount,rw / && mkdir /new-dir-in-root'
  test -d /new-dir-in-root

  test '!' -w '/'

  echo "ok root transient-ro"
  ;;
  *) 
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" 
  ;;
esac
