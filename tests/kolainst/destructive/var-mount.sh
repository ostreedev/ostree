#!/bin/bash
# https://github.com/ostreedev/ostree/issues/1667
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "") 
    touch "/var/somenewfile"
    stateroot=$(ostree admin status 2> /dev/null | grep '^\*' | cut -d ' ' -f2 || true)
    echo "/sysroot/ostree/deploy/${stateroot}/var /var none bind 0 0" >> /etc/fstab
    /tmp/autopkgtest-reboot "2"
    ;;
  "2")
    systemctl status var.mount
    test -f /var/somenewfile
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
