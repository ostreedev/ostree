#!/bin/bash
set -xeuo pipefail

# Add an artificial delay into ostree-finalize-staged.service
# and verify it sees /boot; https://bugzilla.redhat.com/show_bug.cgi?id=1827712

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
"")
dropin=/etc/systemd/system/ostree-finalize-staged.service.d/delay.conf
mkdir -p $(dirname ${dropin})
cat >"${dropin}" << 'EOF'
[Service]
ExecStop=/bin/sh -c 'sleep 10 && if ! test -d /boot/loader/entries; then echo error: no /boot/loader/entries; exit 1; fi; echo ostree-finalize-staged found /boot/loader/entries'
#ExecStop=/bin/false
EOF
systemctl daemon-reload
rpm-ostree kargs --append=somedummykarg=1
/tmp/autopkgtest-reboot 2
;;

"2")
prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
journalctl -b $prev_bootid -u ostree-finalize-staged > logs.txt
assert_file_has_content_literal logs.txt 'ostree-finalize-staged found /boot/loader/entries'
# older systemd doesn't output the success message
if systemctl --version | head -1 | grep -qF -e 'systemd 239'; then
  assert_file_has_content_literal logs.txt 'Stopped OSTree Finalize Staged Deployment'
  assert_not_file_has_content logs.txt 'Failed with result'
else
  assert_file_has_content logs.txt 'ostree-finalize-staged.service: \(Succeeded\|Deactivated successfully\)'
fi
assert_file_has_content_literal /proc/cmdline somedummykarg=1 
;;
*) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
echo ok
