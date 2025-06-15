#!/ bin / bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
#Need to disable gpg verification for test builds
  sed -i -e 's,gpg-verify=true,gpg-verify=false,' /etc/ostree/remotes.d/*.conf

  # Initial cleanup to handle the cosa fast-build case
  ## TODO remove workaround for https://github.com/coreos/rpm-ostree/pull/2021
  mkdir -p /var/lib/rpm-ostree/history
  rpm-ostree cleanup -pr
  commit=${host_commit}

  # Test the prepare-soft-reboot functionality
  cd /ostree/repo/tmp
  # Create a synthetic commit for upgrade
  ostree checkout -H ${commit} t
  # xref https://github.com/coreos/coreos-assembler/pull/2814
  systemctl mask --now zincati
  ostree commit --no-bindings --parent="${commit}" -b soft-reboot-test -I --consume t
  newcommit=$(ostree rev-parse soft-reboot-test)
  
  # Deploy the new commit normally first
  ostree admin deploy soft-reboot-test
  
  # Test prepare-soft-reboot command
  echo "Testing prepare-soft-reboot..."
  
  # Check that we have two deployments now
  deployments_count=$(ostree admin status | grep "S " | wc -l)
  test "${deployments_count}" -eq 1
  
  # Prepare soft reboot for index 0 (the staged deployment)
  ostree admin prepare-soft-reboot 0
  
  # Verify that the sysroot has been prepared for soft reboot
  test -f /run/ostree/soft-reboot-pending
  
  # Use Kola's soft reboot mechanism instead of traditional reboot
  /tmp/autopkgtest-soft-reboot "2"
  ;;
  "2")
  # After soft reboot, verify we're running the new deployment
  echo "Verifying post-soft-reboot state..."
  
  # Check that we booted into the new deployment
  current_commit=$(rpm-ostree status --json | jq -r '.deployments[0].checksum')
  expected_commit=$(ostree rev-parse soft-reboot-test)
  
  if [ "${current_commit}" != "${expected_commit}" ]; then
    echo "ERROR: Expected commit ${expected_commit}, but got ${current_commit}"
    exit 1
  fi
  
  # Verify that soft-reboot-pending file is cleaned up
  test '!' -f /run/ostree/soft-reboot-pending
  
  echo "Soft reboot test completed successfully!"
  
  # Cleanup
  ostree refs --delete soft-reboot-test
  rpm-ostree cleanup -pr
  ;;
  *) 
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" 
  ;;
esac