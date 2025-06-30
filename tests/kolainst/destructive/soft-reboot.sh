#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

echo "testing boot=${AUTOPKGTEST_REBOOT_MARK:-}"
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

  assert_status_jq '.deployments[0].staged' '.deployments[0].["soft-reboot-target"] | not'

  # Test prepare-soft-reboot command
  echo "Testing prepare-soft-reboot..."
  ostree admin prepare-soft-reboot 0

  # Test human readable format
  ostree admin status > status.txt
  assert_file_has_content_literal status.txt '(staged) (soft-reboot)'

  # And via JSON
  assert_status_jq '.deployments[0].pending' '.deployments[0].["soft-reboot-target"]'

  # Verify the internal state
  test -f /run/ostree/nextroot-queued
  # This one now only exists temporarily between shutdown+reboot
  test '!' -f /run/ostree/nextroot-booted
  if mountpoint /run/nextroot 2>/dev/null; then fatal "/run/nextroot should not be mounted"; fi
  
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
  
  # Verify that soft-reboot state files are gone
  test '!' -f /run/ostree/nextroot-queued
  test '!' -f /run/ostree/nextroot-booted
  
  echo "Soft reboot test completed successfully!"

  # Now soft reboot again into the rollback which is not staged,
  # and also exercise the immediate --reboot flag.
  touch /etc/current-contents
  /tmp/autopkgtest-soft-reboot-prepare "3"
  ostree admin prepare-soft-reboot --reboot 1
  ;;
  "3")

  # Only from the first updated target
  test '!' -f /etc/new-file-for-soft-reboot
  test '!' -f /usr/share/test-file-for-soft-reboot
  # And this was in the *previous* current /etc
  test '!' -f /etc/current-contents

  echo "ok verified prepare"

  assert_status_jq '.deployments[0].["soft-reboot-target"] | not'

  ostree admin prepare-soft-reboot 0
  assert_status_jq '.deployments[0].["soft-reboot-target"]'
  ostree admin prepare-soft-reboot --reset
  assert_status_jq '.deployments[0].["soft-reboot-target"] | not'
  test '!' -f /run/ostree/nextroot-queued
  # Test idempotence
  ostree admin prepare-soft-reboot --reset
  assert_status_jq '.deployments[0].["soft-reboot-target"] | not'
  test '!' -f /run/ostree/nextroot-queued

  echo "ok soft reboot"
  ;;
  *) 
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" 
  ;;
esac
