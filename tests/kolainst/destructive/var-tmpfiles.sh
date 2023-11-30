#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
    factory_var=rootfs/usr/share/factory/var
    mkdir -p ${factory_var}
    cd "${factory_var}"
    mkdir -p home/someuser
    echo bashrc > home/someuser/.bashrc
    chown -R 1000:1000 home/someuser
    mkdir -m 01777 -p tmp
    cd -
    ostree commit -b testlint --no-bindings --selinux-policy-from-base --tree=ref="${host_refspec}" --consume --tree=dir=rootfs
    ostree admin deploy testlint 2>err.txt
    assert_not_file_has_content err.txt 'contains content in /var'

    /tmp/autopkgtest-reboot "2"
    ;;
  2)
    assert_file_has_content /home/someuser/.bashrc bashrc
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
