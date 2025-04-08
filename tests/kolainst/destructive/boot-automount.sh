#!/bin/bash
# https://github.com/ostreedev/ostree/issues/2543
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
    # Ensure boot is mount point
    mountpoint /boot

    # Create an automount unit with an extremely short timeout
    cat > /etc/systemd/system/boot.automount <<"EOF"
[Automount]
Where=/boot
TimeoutIdleSec=1

[Install]
WantedBy=local-fs.target
EOF
    systemctl daemon-reload
    systemctl enable boot.automount

    # Stop this as it may also be holding /boot open now
    systemctl stop rpm-ostreed.service

    # Unmount /boot, start the automount unit, and ensure the units are
    # in the correct states.
    umount /boot
    systemctl start boot.automount
    boot_state=$(systemctl show -P ActiveState boot.mount)
    boot_auto_state=$(systemctl show -P ActiveState boot.automount)
    assert_streq "${boot_state}" inactive
    assert_streq "${boot_auto_state}" active

    # Trigger a new staged deployment and check that the relevant units
    # are enabled.
    ostree admin deploy --stage --karg-append=somedummykarg=1 "${host_commit}"
    rpm-ostree status --json
    deployment_staged=$(rpmostree_query_json '.deployments[0].staged')
    assert_streq "${deployment_staged}" true
    test -f /run/ostree/staged-deployment
    finalize_staged_state=$(systemctl show -P ActiveState ostree-finalize-staged.service)
    finalize_staged_hold_state=$(systemctl show -P ActiveState ostree-finalize-staged-hold.service)
    assert_streq "${finalize_staged_state}" active
    assert_streq "${finalize_staged_hold_state}" active

    # Sleep 1 second to ensure the boot automount idle timeout has
    # passed and then check that /boot is still mounted.
    sleep 1
    boot_state=$(systemctl show -P ActiveState boot.mount)
    assert_streq "${boot_state}" active

    /tmp/autopkgtest-reboot "2"
    ;;
  "2")
    rpm-ostree status --json
    deployment_staged=$(rpmostree_query_json '.deployments[0].staged')
    assert_streq "${deployment_staged}" false
    test ! -f /run/ostree/staged-deployment
    assert_file_has_content_literal /proc/cmdline somedummykarg=1

    # Check that the finalize and hold services succeeded in the
    # previous boot. Dump them to the test log to help debugging.
    prepare_tmpdir
    prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
    journalctl -b "${prev_bootid}" -o short-monotonic \
        -u ostree-finalize-staged.service \
        -u ostree-finalize-staged-hold.service \
        -u boot.mount \
        -u boot.automount \
        > logs.txt
    cat logs.txt
    assert_file_has_content logs.txt 'ostree-finalize-staged.service: \(Succeeded\|Deactivated successfully\)'
    assert_file_has_content logs.txt 'ostree-finalize-staged-hold.service: \(Succeeded\|Deactivated successfully\)'

    # Check that the hold service remained active and kept /boot mounted until
    # the finalize service completed.
    prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
    finalize_stopped=$(journalctl -b $prev_bootid -o json -g Stopped -u ostree-finalize-staged.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    hold_stopping=$(journalctl -b $prev_bootid -o json -g Stopping -u ostree-finalize-staged-hold.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    hold_stopped=$(journalctl -b $prev_bootid -o json -g Stopped -u ostree-finalize-staged-hold.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    boot_unmounting=$(journalctl -b $prev_bootid -o json -g Unmounting -u boot.mount | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    test "${finalize_stopped}" -lt "${hold_stopping}"
    test "${hold_stopped}" -lt "${boot_unmounting}"
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
