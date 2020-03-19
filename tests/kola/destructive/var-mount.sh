#!/bin/bash
# https://github.com/ostreedev/ostree/issues/1667
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

n=$(nth_boot)
case "${n}" in
  1) 
    require_writable_sysroot
    # Hack this off for now
    chattr -i /sysroot
    cp -a /var /sysroot/myvar
    touch /sysroot/myvar/somenewfile
    echo '/sysroot/myvar /var none bind 0 0' >> /etc/fstab
    kola_reboot
    ;;
  2)
    systemctl status var.mount
    test -f /var/somenewfile
    ;;
  *) fatal "Unexpected boot count $n"
esac
