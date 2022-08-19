#!/bin/bash

# Verify "ostree admin kargs edit-in-place" works

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
"")
  sudo rpm-ostree kargs --append=somedummykarg=1
  sudo ostree admin kargs edit-in-place --append-if-missing=testarg
  assert_file_has_content /boot/loader/entries/ostree-* testarg
  /tmp/autopkgtest-reboot "2"
  ;;
"2")
  assert_file_has_content_literal /proc/cmdline somedummykarg=1
  assert_file_has_content_literal /proc/cmdline testarg
  echo "ok test with stage: kargs edit-in-place --append-if-missing"
  ;;
*)
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}"
  ;;
esac
