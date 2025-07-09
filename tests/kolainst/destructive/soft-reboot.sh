#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

prepare_tmpdir

echo "testing boot=${AUTOPKGTEST_REBOOT_MARK:-}"

# Print this by default on each boot
ostree admin status

# Verify /sysroot readonly by default on each boot
test '!' -w /sysroot
findmnt -J /sysroot > findmnt.json
assert_jq findmnt.json '.filesystems[0].options | contains("ro")'
# But mount it writable now so we can make test commits conveniently
require_writable_sysroot

assert_soft_reboot_count() {
  assert_streq $(systemctl show -P SoftRebootsCount) $1
}

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # xref https://github.com/coreos/coreos-assembler/pull/2814
  systemctl mask --now zincati

  assert_soft_reboot_count 0
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

  test -f /run/ostree/nextroot-booted
  mountpoint /run/nextroot
  
  /tmp/autopkgtest-soft-reboot "2"
  ;;
  "2")
  # After soft reboot, verify we're running the new deployment
  echo "Verifying post-soft-reboot state..."
  assert_soft_reboot_count 1
  
  expected_commit=$(ostree rev-parse soft-reboot-test)
  
  if [ "${host_commit}" != "${expected_commit}" ]; then
    echo "ERROR: Expected commit ${host_commit}, but got ${current_commit}"
    exit 1
  fi

  assert_status_jq '.deployments[0].booted' '.deployments[0].["soft-reboot-target"] | not'

  test -f /etc/new-file-for-soft-reboot
  test -f /usr/share/test-file-for-soft-reboot
  
  # Verify that soft-reboot state files are gone
  test '!' -f /run/ostree/nextroot-booted
  
  echo "Soft reboot test completed successfully!"

  # Now soft reboot again into the rollback which is not staged,
  # and also exercise the immediate --reboot flag.
  touch /etc/current-contents
  /tmp/autopkgtest-soft-reboot-prepare "3"
  ostree admin prepare-soft-reboot --reboot 1
  ;;
  "3")
  assert_soft_reboot_count 2

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
  test '!' -f /run/ostree/nextroot-booted
  # Test idempotence
  ostree admin prepare-soft-reboot --reset
  assert_status_jq '.deployments[0].["soft-reboot-target"] | not'
  test '!' -f /run/ostree/nextroot-booted

  echo "ok soft reboot 3"

  # Now, test the intersection of staged deployments and soft rebooting
  # Create another synthetic commit
  cd /ostree/repo/tmp
  ostree checkout -H ${host_commit} t
  unshare -m /bin/sh -c 'mount -o remount,rw /sysroot && cd /ostree/repo/tmp/t && touch usr/share/test-staged-2-for-soft-reboot'
  ostree commit --no-bindings --parent="${host_commit}" -b soft-reboot-test-staged-2 -I --consume t
  newcommit=$(ostree rev-parse soft-reboot-test-staged-2)
  ostree admin deploy --stage soft-reboot-test-staged-2

  assert_status_jq '.deployments[0].staged' '.deployments[0].["soft-reboot-target"] | not' \
                   '.deployments[1].booted | not' '.deployments[1].["soft-reboot-target"] | not' \
                   '.deployments[2].booted' '.deployments[2].["soft-reboot-target"] | not'

  # Note here we're targeting the previous booted deployment, *not* the staged
  ostree admin prepare-soft-reboot 1

  # We set up the soft reboot, but cleared the staged
  assert_status_jq '.deployments[0].booted | not' '.deployments[0].["soft-reboot-target"]' \
                   '.deployments[1].booted' '.deployments[1].["soft-reboot-target"] | not'

  # And this one verifies we do a soft reboot by default as we've mounted /run/nextroot
  /tmp/autopkgtest-soft-reboot-prepare "4"
  systemctl reboot
  ;;
  "4")
  assert_soft_reboot_count 3
  # Completion of soft reboot into non-staged
  assert_status_jq '.deployments[0].booted' '.deployments[0].["soft-reboot-target"] | not' \
                   '.deployments[1].booted | not' '.deployments[1].["soft-reboot-target"] | not'
  echo "ok soft reboot to non-staged"

  # We can't soft reboot into a changed kernel state
  rpm-ostree initramfs --enable
  if ostree admin prepare-soft-reboot 0 2>err.txt; then
    fatal "soft reboot prep with kernel change"
  fi
  assert_file_has_content_literal err.txt "different kernel state"
  rm -vf err.txt

  rpm-ostree cleanup -p

  rpm-ostree kargs --append=foo=bar
  if ostree admin prepare-soft-reboot 0 2>err.txt; then
    fatal "soft reboot prep with kernel args change"
  fi
  assert_file_has_content_literal err.txt "different kernel state"

  echo "ok soft reboot all tests"
  ;;
  *) 
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" 
  ;;
esac
