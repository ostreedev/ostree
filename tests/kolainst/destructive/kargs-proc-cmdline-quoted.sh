#!/bin/bash

# Verify ostree handles quoted kernel args with spaces in /proc/cmdline.
#
# Regression test for a bug introduced in abc7d5b9 where
# ostree_kernel_args_append_proc_cmdline() used a naive g_strsplit(" ")
# to tokenize /proc/cmdline, then passed the fragments to
# ostree_kernel_args_append() which calls the quote-aware
# split_kernel_args(). Fragments with unterminated quotes caused a
# g_assert_false(quoted) abort.
#
# The bootloader (GRUB) may reformat quoted kargs -- e.g. what
# rpm-ostree stores as testparam="value with spaces" can appear in
# /proc/cmdline as "testparam=value with spaces" (quotes wrapping
# the entire token). The parser must handle both forms.

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
"")
  # Append a quoted karg containing spaces
  rpm-ostree kargs --append='testparam="value with spaces"'
  /tmp/autopkgtest-reboot "verify"
  ;;
"verify")
  # The karg should be present in /proc/cmdline (possibly reformatted
  # by the bootloader, e.g. "testparam=value with spaces")
  assert_file_has_content /proc/cmdline testparam

  # These commands read /proc/cmdline and previously crashed with:
  #   g_assert_false(quoted) in split_kernel_args()
  ostree admin instutil set-kargs --import-proc-cmdline
  echo "ok import-proc-cmdline with quoted kargs"

  host_commit=$(ostree admin status | sed -n 's/^.* \(.*\)\.0$/\1/p' | head -1)
  ostree admin deploy --karg-proc-cmdline "${host_commit}"
  echo "ok deploy --karg-proc-cmdline with quoted kargs"
  ;;
*)
  fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}"
  ;;
esac
