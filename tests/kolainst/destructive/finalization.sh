#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # Need to disable gpg verification for test builds
  sed -i -e 's,gpg-verify=true,gpg-verify=false,' /etc/ostree/remotes.d/*.conf
  # xref https://github.com/coreos/coreos-assembler/pull/2814
  systemctl mask --now zincati

  # Create a synthetic commit for upgrade
  ostree commit --no-bindings --parent="${host_commit}" -b staged-deploy -I --tree=ref="${host_commit}"
  newcommit=$(ostree rev-parse staged-deploy)
  ostree admin deploy --lock-finalization staged-deploy
  systemctl show -p ActiveState ostree-finalize-staged.service | grep active
  ostree admin status > status.txt
  assert_file_has_content status.txt 'finalization locked'
  # Because finalization was locked, we shouldn't deploy on shutdown
  /tmp/autopkgtest-reboot "2"
  ;;
  "2") 
  # Verify we didn't finalize
  newcommit=$(ostree rev-parse staged-deploy)
  booted=$(rpm-ostree status --json | jq -r .deployments[0].checksum)
  assert_not_streq "${newcommit}" "${booted}"
  prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
  journalctl -b $prev_bootid -u ostree-finalize-staged.service > svc.txt
  assert_file_has_content svc.txt 'Not finalizing'
  ostree admin status > status.txt
  assert_not_file_has_content status.txt 'finalization locked'

  # Now re-deploy
  ostree admin deploy --lock-finalization staged-deploy
  ostree admin status > status.txt
  assert_file_has_content status.txt 'finalization locked'
  # And unlock
  ostree admin lock-finalization --unlock
  ostree admin status > status.txt
  assert_not_file_has_content status.txt 'finalization locked'

  /tmp/autopkgtest-reboot "3"
  ;;
  "3") 
  newcommit=$(ostree rev-parse staged-deploy)
  booted=$(rpm-ostree status --json | jq -r .deployments[0].checksum)
  assert_streq "${newcommit}" "${booted}"
  prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
  journalctl -b $prev_bootid -u ostree-finalize-staged.service > svc.txt
  assert_file_has_content svc.txt 'Bootloader updated'
  ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
