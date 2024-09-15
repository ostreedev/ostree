#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

bootupd_state=/boot/bootupd-state.json
mount -o remount,rw /boot
if grep -qFe "\"static-configs\"" "${bootupd_state}"; then
    echo "Host is using static configs already, overriding this"
    jq --compact-output '.["static-configs"] = null' < "${bootupd_state}" > "${bootupd_state}".new
    mv "${bootupd_state}.new" "${bootupd_state}"
fi

# Print the current value for reference, it's "none" on FCOS derivatives
ostree config get sysroot.bootloader || true
ostree config set sysroot.bootloader auto

ostree admin deploy --stage "${host_commit}"
systemctl stop ostree-finalize-staged.service
used_bootloader=$(journalctl -u ostree-finalize-staged -o json MESSAGE_ID=dd440e3e549083b63d0efc7dc15255f1 | tail -1 | jq -r .OSTREE_BOOTLOADER)
# We're verifying the legacy default now
assert_streq "${used_bootloader}" "grub2"
ostree admin undeploy 0

# Now synthesize a bootupd config which uses static configs
jq '. + {"static-configs": {}}' < "${bootupd_state}"  > "${bootupd_state}".new
mv "${bootupd_state}.new" "${bootupd_state}"
ostree admin deploy --stage "${host_commit}"
systemctl stop ostree-finalize-staged.service
used_bootloader=$(journalctl -u ostree-finalize-staged -o json MESSAGE_ID=dd440e3e549083b63d0efc7dc15255f1 | tail -1 | jq -r .OSTREE_BOOTLOADER)
assert_streq "${used_bootloader}" "none"

echo "ok bootupd static"
