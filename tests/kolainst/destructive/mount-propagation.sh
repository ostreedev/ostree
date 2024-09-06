#!/bin/bash
# https://bugzilla.redhat.com/show_bug.cgi?id=1498281
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot

get_mount() {
  local target=${1:?No target specified}
  local pid=${2:?No PID specified}

  # findmnt always looks at /proc/self/mountinfo, so we have to first enter the
  # mount namespace of the desired process.
  nsenter --target "${pid}" --mount -- \
      findmnt --json --list --output +PROPAGATION,OPT-FIELDS \
      | jq ".filesystems[] | select(.target == \"${target}\")"
}

assert_has_mount() {
  local target=${1:?No target specified}
  local pid=${2:-$$}
  local mount

  mount=$(get_mount "${target}" "${pid}")
  if [ -n "${mount}" ]; then
    echo -e "Process ${pid} has mount '${target}':\n${mount}"
  else
    cat "/proc/${pid}/mountinfo" >&2
    fatal "Mount '${target}' not found in process ${pid}"
  fi
}

assert_not_has_mount() {
  local target=${1:?No target specified}
  local pid=${2:-$$}
  local mount

  mount=$(get_mount "${target}" "${pid}")
  if [ -n "${mount}" ]; then
    cat "/proc/${pid}/mountinfo" >&2
    fatal "Mount '${target}' found in process ${pid}"
  else
    echo "Process ${pid} does not have mount '${target}'"
  fi
}

test_mounts() {
  local stateroot

  echo "Root namespace mountinfo:"
  cat "/proc/$$/mountinfo"

  echo "Sub namespace mountinfo:"
  cat "/proc/${ns_pid}/mountinfo"

  # Make sure the 2 PIDs are really in different mount namespaces.
  root_ns=$(readlink "/proc/$$/ns/mnt")
  sub_ns=$(readlink "/proc/${ns_pid}/ns/mnt")
  assert_not_streq "${root_ns}" "${sub_ns}"

  stateroot=$(rpmostree_query_json '.deployments[0].osname')

  # Check the mounts exist in the root namespace and the /var/foo mount has not
  # propagated back to /sysroot.
  assert_has_mount /var/foo
  assert_has_mount /sysroot/bar
  assert_not_has_mount "/sysroot/ostree/deploy/${stateroot}/var/foo"

  # Repeat with the sub mount namespace.
  assert_has_mount /var/foo "${ns_pid}"
  assert_has_mount /sysroot/bar "${ns_pid}"
  assert_not_has_mount "/sysroot/ostree/deploy/${stateroot}/var/foo" "${ns_pid}"
}

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
    mkdir -p /var/foo /sysroot/bar

    # Create a process in a separate mount namespace to see if the mounts
    # propagate into it correctly.
    unshare -m --propagation unchanged -- sleep infinity &
    ns_pid=$!

    mount -t tmpfs foo /var/foo
    mount -t tmpfs bar /sysroot/bar

    test_mounts

    # Now setup for the same test but with the mounts made early via fstab.
    cat >> /etc/fstab <<"EOF"
foo /var/foo tmpfs defaults 0 0
bar /sysroot/bar tmpfs defaults 0 0
EOF

    # We want to start a process in a separate namespace after ostree-remount
    # has completed but before systemd starts the fstab generated mount units.
    cat > /etc/systemd/system/test-mounts.service <<"EOF"
[Unit]
DefaultDependencies=no
After=ostree-remount.service
Before=var-foo.mount sysroot-bar.mount
RequiresMountsFor=/var /sysroot
Conflicts=shutdown.target
Before=shutdown.target

[Service]
Type=exec
ExecStart=/usr/bin/sleep infinity
ProtectSystem=strict

[Install]
WantedBy=local-fs.target
EOF
    systemctl enable test-mounts.service

    /tmp/autopkgtest-reboot 2
    ;;
  2)
    # Check that the test service is running and get its PID.
    ns_state=$(systemctl show -P ActiveState test-mounts.service)
    assert_streq "${ns_state}" active
    ns_pid=$(systemctl show -P MainPID test-mounts.service)

    # Make sure that test-mounts.service started after ostree-remount.service
    # but before /var/foo and /sysroot/bar were mounted so that we can see if
    # the mounts were propagated into its mount namespace.
    remount_finished=$(journalctl -o json -g Finished -u ostree-remount.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    test_starting=$(journalctl -o json -g Starting -u test-mounts.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    test_started=$(journalctl -o json -g Started -u test-mounts.service | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    foo_mounting=$(journalctl -o json -g Mounting -u var-foo.mount | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    bar_mounting=$(journalctl -o json -g Mounting -u sysroot-bar.mount | tail -n1 | jq -r .__MONOTONIC_TIMESTAMP)
    test "${remount_finished}" -lt "${test_starting}"
    test "${test_started}" -lt "${foo_mounting}"
    test "${test_started}" -lt "${bar_mounting}"

    test_mounts
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
