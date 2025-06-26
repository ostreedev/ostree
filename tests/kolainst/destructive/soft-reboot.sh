#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # xref https://github.com/coreos/coreos-assembler/pull/2814
  systemctl mask --now zincati

  assert_streq $(systemctl show -P SoftRebootsCount) 0
  assert_status_jq '.deployments[0].pending | not' '.deployments[0].["soft-reboot-target"] | not'

  # Create a synthetic commit for upgrade
  cd /ostree/repo/tmp
  ostree checkout -H ${host_commit} t
  unshare -m /bin/sh -c 'mount -o remount,rw /sysroot && cd /ostree/repo/tmp/t && touch usr/etc/new-file-for-soft-reboot usr/share/test-file-for-soft-reboot'
  ostree commit --no-bindings --parent="${host_commit}" -b soft-reboot-test -I --consume t
  newcommit=$(ostree rev-parse soft-reboot-test)
  # Deploy the new commit normally first
  ostree admin deploy --stage soft-reboot-test

  assert_status_jq '.deployments[0].pending' '.deployments[0].["soft-reboot-target"] | not'

  # Test prepare-soft-reboot command
  echo "Testing prepare-soft-reboot..."
  ostree admin prepare-soft-reboot 0

  # Test human readable format
  ostree admin status > status.txt
  assert_file_has_content_literal status.txt '(pending) (soft-reboot)'

  # And via JSON
  assert_status_jq '.deployments[0].pending' '.deployments[0].["soft-reboot-target"]'

  # Verify the internal state file
  test -f /run/ostree/nextroot-booted
  
  /tmp/autopkgtest-soft-reboot "2"
  ;;
  "2")
  # After soft reboot, verify we're running the new deployment
  echo "Verifying post-soft-reboot state..."
  assert_streq $(systemctl show -P SoftRebootsCount) 1
  
  expected_commit=$(ostree rev-parse soft-reboot-test)
  
  if [ "${host_commit}" != "${expected_commit}" ]; then
    echo "ERROR: Expected commit ${host_commit}, but got ${current_commit}"
    exit 1
  fi

  assert_status_jq '.deployments[0].booted' '.deployments[0].["soft-reboot-target"] | not'

  test -f /etc/new-file-for-soft-reboot
  test -f /usr/share/test-file-for-soft-reboot
  
  # Verify that soft-reboot-pending file is cleaned up
  test '!' -f /run/ostree/nextroot-booted
  
  echo "Soft reboot test completed successfully!"
  ;;
  *) 
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" 
  ;;
esac
