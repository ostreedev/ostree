#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # Need to disable gpg verification for test builds
  sed -i -e 's,gpg-verify=true,gpg-verify=false,' /etc/ostree/remotes.d/*.conf

  # Test our generator
  test -f /run/systemd/generator/local-fs.target.requires/ostree-remount.service

  cat >/etc/systemd/system/sock-to-ignore.socket << 'EOF'
[Socket]
ListenStream=/etc/sock-to-ignore
EOF
  cat >/etc/systemd/system/sock-to-ignore.service << 'EOF'
[Service]
ExecStart=/bin/cat
EOF
  # policy denies systemd listening on a socket in /etc (arguably correctly)
  setenforce 0
  systemctl daemon-reload
  systemctl start --now sock-to-ignore.socket
  setenforce 1

  test -S /etc/sock-to-ignore
  mkfifo /etc/fifo-to-ignore

  # Initial cleanup to handle the cosa fast-build case
    ## TODO remove workaround for https://github.com/coreos/rpm-ostree/pull/2021
    mkdir -p /var/lib/rpm-ostree/history
    rpm-ostree cleanup -pr

    ostree admin status --json > status.json
    jq -e '.deployments[0].staged | not' status.json

    commit=${host_commit}
  # Test the deploy --stage functionality; first, we stage a deployment
  # reboot, and validate that it worked.
  # Write staged-deploy commit
    cd /ostree/repo/tmp
    # https://github.com/ostreedev/ostree/issues/1569
    ostree checkout -H ${commit} t
    # xref https://github.com/coreos/coreos-assembler/pull/2814
    systemctl mask --now zincati
    # Create a synthetic commit for upgrade
    ostree commit --no-bindings --parent="${commit}" -b staged-deploy -I --consume t
    newcommit=$(ostree rev-parse staged-deploy)
    orig_mtime=$(stat -c '%.Y' /sysroot/ostree/deploy)
    systemctl show -p ActiveState ostree-finalize-staged.service | grep -q inactive

    syncfs=$(journalctl --grep='Completed syncfs.*for system repo' | wc -l)
    assert_streq "$syncfs" 2

    # Do the staged deployment
    ostree admin deploy --stage staged-deploy

    # Verify state
    ostree admin status --json > status.json
    jq -e '.deployments[0].staged' status.json
    systemctl show -p ActiveState ostree-finalize-staged.service | grep active
    new_mtime=$(stat -c '%.Y' /sysroot/ostree/deploy)
    test "${orig_mtime}" != "${new_mtime}"
    test -f /run/ostree/staged-deployment
    ostree refs | grep -E -e '^ostree/' | while read ref; do
      if test "$(ostree rev-parse ${ref})" = "${newcommit}"; then
        touch deployment-ref-found
      fi
    done
    test -f deployment-ref-found
    rm deployment-ref-found
    if ostree admin pin 0 2>err.txt; then
      fatal "Pinned staged deployment"
    fi
    assert_file_has_content err.txt 'Cannot pin staged deployment'
    /tmp/autopkgtest-reboot "2"
    ;;
  "2") 
    # Check that deploy-staged service worked
    rpm-ostree status
    # Assert that the previous boot had a journal entry for it
    prev_bootid=$(journalctl --list-boots -o json |jq -r '.[] | select(.index == -1) | .boot_id')
    journalctl -b $prev_bootid -u ostree-finalize-staged.service > svc.txt
    assert_file_has_content svc.txt 'Bootloader updated; bootconfig swap: yes;.*deployment count change: 1'
    # Also validate ignoring socket and fifo
    assert_file_has_content svc.txt 'Ignoring.*during /etc merge:.*sock-to-ignore'
    assert_file_has_content svc.txt 'Ignoring.*during /etc merge:.*fifo-to-ignore'
    rm -f svc.txt
    # And there should not be a staged deployment
    test '!' -f /run/ostree/staged-deployment

    # Also verify we did a syncfs run during finalization
    syncfs=$(journalctl -b -1 -u ostree-finalize-staged --grep='Completed syncfs.*for system repo' | wc -l)
    assert_streq "$syncfs" 2

    test '!' -f /run/ostree/staged-deployment
    ostree admin status > status.txt
    assert_not_file_has_content status.txt 'finalization locked'
    ostree admin deploy staged-deploy --lock-finalization
    ostree admin status > status.txt
    assert_file_has_content status.txt 'finalization locked'
    test -f /run/ostree/staged-deployment
    # check that we can cleanup the staged deployment
    ostree admin undeploy 0
    test ! -f /run/ostree/staged-deployment
    echo "ok cleanup staged"

    # And verify that re-staging cleans the previous lock
    test '!' -f /run/ostree/staged-deployment
    ostree admin deploy --stage staged-deploy
    test -f /run/ostree/staged-deployment
    test '!' -f /run/ostree/staged-deployment-locked
    ostree admin undeploy 0
    echo "ok restage unlocked"

    # Upgrade with staging
    ostree admin deploy --stage staged-deploy
    origcommit=$(ostree rev-parse staged-deploy)
    cd /ostree/repo/tmp
    ostree checkout -H "${origcommit}" t
    ostree commit --no-bindings --parent="${origcommit}" -b staged-deploy -I --consume t
    newcommit=$(ostree rev-parse staged-deploy)
    ostree admin upgrade --stage >out.txt
    test -f /run/ostree/staged-deployment
    firstdeploycommit=$(rpm-ostree status --json | jq -r .deployments[0].checksum)
    assert_streq "${firstdeploycommit}" "${newcommit}"
    # Cleanup
    rpm-ostree cleanup -rp
    echo "ok upgrade with staging"

    # Ensure we can unstage
    # Write staged-deploy commit, then unstage
    ostree admin deploy --stage staged-deploy
    ostree admin status > status.txt
    assert_file_has_content_literal status.txt '(staged)'
    test -f /run/ostree/staged-deployment
    ostree admin undeploy 0
    ostree admin status > status.txt
    grep -vqFe '(staged)' status.txt
    test '!' -f /run/ostree/staged-deployment
    echo "ok unstage"

    ostree admin deploy staged-deploy --lock-finalization
    ostree admin status > status.txt
    assert_file_has_content status.txt 'finalization locked'
    ostree admin lock-finalization > out.txt
    assert_file_has_content_literal out.txt 'already finalization locked'
    ostree admin status > status.txt
    assert_file_has_content status.txt 'finalization locked'
    ostree admin lock-finalization -u > out.txt
    assert_file_has_content_literal out.txt 'now queued to apply'
    ostree admin status > status.txt
    assert_not_file_has_content status.txt 'finalization locked'
    echo "ok finalization locking toggle"

   # Staged should be overwritten by non-staged as first
    commit=$(rpmostree_query_json '.deployments[0].checksum')
    ostree admin deploy --stage staged-deploy
    test -f /run/ostree/staged-deployment
    ostree --repo=/ostree/repo refs --create nonstaged-deploy "${commit}"
    ostree admin deploy nonstaged-deploy
    ostree admin status > status.txt
    grep -vqFe '(staged)' status.txt
    test '!' -f /run/ostree/staged-deployment
    ostree admin undeploy 0
    echo "ok staged overwritten by non-staged"

  # Staged is retained when pushing rollback
    commit=$(rpmostree_query_json '.deployments[0].checksum')
    ostree admin deploy --stage staged-deploy
    test -f /run/ostree/staged-deployment
    ostree admin deploy --retain-pending --not-as-default nonstaged-deploy
    test -f /run/ostree/staged-deployment
    ostree admin status > status.txt
    assert_file_has_content_literal status.txt '(staged)'
    ostree admin undeploy 0
    ostree admin undeploy 1
    echo "ok staged retained"

    # Cleanup refs
    ostree refs --delete staged-deploy nonstaged-deploy
    echo "ok cleanup refs"

    # Now finally, try breaking staged updates and verify that ostree-boot-complete fails on the next boot
    unshare -m /bin/sh -c 'mount -o remount,rw /boot; chattr +i /boot'
    rpm-ostree kargs --append=foo=bar

    # Hack around https://github.com/coreos/coreos-assembler/pull/2921#issuecomment-1156592723
    # where coreos-assembler/kola check systemd unit status right after ssh.
    cat >/etc/systemd/system/hackaround-cosa-systemd-unit-checks.service << 'EOF'
[Unit]
Before=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c '(systemctl status ostree-boot-complete.service || true) | tee /run/ostree-boot-complete-status.txt'
ExecStart=/bin/systemctl reset-failed ostree-boot-complete.service

[Install]
WantedBy=multi-user.target
EOF
    systemctl enable hackaround-cosa-systemd-unit-checks.service

    /tmp/autopkgtest-reboot "3"
    ;;
  "3") 
    assert_file_has_content /run/ostree-boot-complete-status.txt 'error: ostree-finalize-staged.service failed on previous boot.*Operation not permitted'
    echo "ok boot-complete.service"
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
