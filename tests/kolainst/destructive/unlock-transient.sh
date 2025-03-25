#!/bin/bash
# Test unlock --transient
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

testfile=/usr/share/writable-usr-test

stateroot=$(rpmostree_query_json '.deployments[0].osname')
checksum=$(rpmostree_query_json '.deployments[0].checksum')
serial=$(rpmostree_query_json '.deployments[0].serial')
backing=/ostree/deploy/${stateroot}/backing/$checksum.$serial/usr-transient

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "") 
    require_writable_sysroot
    assert_not_has_file "${testfile}"
    ostree admin unlock --transient
    # It's still read-only
    if touch ${testfile}; then
      fatal "modified /usr"
    fi
    # But, we can affect it in a new mount namespace
    unshare -m -- /bin/sh -c 'mount --options-source=disable -o remount,rw /usr && echo hello from transient unlock >'"${testfile}"
    assert_file_has_content "${testfile}" "hello from transient unlock"
    # Still can't write to it from the outer namespace
    if touch ${testfile} || rm -v "${testfile}" 2>/dev/null; then
      fatal "modified ${testfile}"
    fi
    # And the file should be written to the backing dir, note the
    # /usr got stripped.
    test -f ${backing}/upper/share/writable-usr-test
    /tmp/autopkgtest-reboot 2
    ;;
  "2")
    if test -f "${testfile}"; then
      fatal "${testfile} persisted across reboot?"
    fi
    ostree admin unlock --transient
    # Test again to ensure we didn't leak state across reboot
    if test -f "${testfile}"; then
      fatal "${testfile} persisted across reboot?"
    fi
    echo "ok unlock transient"
    ;;
  *) fatal "Unexpected boot mark ${AUTOPKGTEST_REBOOT_MARK}"
esac
