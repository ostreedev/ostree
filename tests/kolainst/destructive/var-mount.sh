#!/bin/bash
# https://github.com/ostreedev/ostree/issues/1667
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "") 
    require_writable_sysroot
    # Hack this off for now
    chattr -i /sysroot
    cp -a /var /sysroot/myvar
    touch /sysroot/myvar/somenewfile
    echo '/sysroot/myvar /var none bind 0 0' >> /etc/fstab
    /tmp/autopkgtest-reboot "2"
    ;;
  "2")
    systemctl status var.mount
    test -f /var/somenewfile
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
