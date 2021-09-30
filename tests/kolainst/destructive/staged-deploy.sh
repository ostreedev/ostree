#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot
prepare_tmpdir

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
  # Test our generator
  test -f /run/systemd/generator/multi-user.target.wants/ostree-finalize-staged.path
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
    commit=${host_commit}
  # Test the deploy --stage functionality; first, we stage a deployment
  # reboot, and validate that it worked.
  # Write staged-deploy commit
    cd /ostree/repo/tmp
    # https://github.com/ostreedev/ostree/issues/1569
    ostree checkout -H ${commit} t
    ostree commit --no-bindings --parent="${commit}" -b staged-deploy -I --consume t
    newcommit=$(ostree rev-parse staged-deploy)
    orig_mtime=$(stat -c '%.Y' /sysroot/ostree/deploy)
    systemctl show -p SubState ostree-finalize-staged.path | grep -q waiting
    systemctl show -p ActiveState ostree-finalize-staged.service | grep -q inactive
    systemctl show -p TriggeredBy ostree-finalize-staged.service | grep -q path
    ostree admin deploy --stage staged-deploy
    systemctl show -p SubState ostree-finalize-staged.path | grep running
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
    journalctl -b "-1" -u ostree-finalize-staged.service > svc.txt
    assert_file_has_content svc.txt 'Bootloader updated; bootconfig swap: yes;.*deployment count change: 1'
    # Also validate ignoring socket and fifo
    assert_file_has_content svc.txt 'Ignoring.*during /etc merge:.*sock-to-ignore'
    assert_file_has_content svc.txt 'Ignoring.*during /etc merge:.*fifo-to-ignore'
    rm -f svc.txt
    # And there should not be a staged deployment
    test '!' -f /run/ostree/staged-deployment

    # Upgrade with staging
    test '!' -f /run/ostree/staged-deployment
    ostree admin deploy --stage staged-deploy
    test -f /run/ostree/staged-deployment
    origcommit=$(ostree rev-parse staged-deploy)
    cd /ostree/repo/tmp
    ostree checkout -H "${origcommit}" t
    ostree commit --no-bindings --parent="${origcommit}" -b staged-deploy -I --consume t
    newcommit=$(ostree rev-parse staged-deploy)
    ostree admin upgrade --stage >out.txt
    test -f /run/ostree/staged-deployment
    # Debating bouncing back out to Ansible for this
    firstdeploycommit=$(rpm-ostree status |grep 'Commit:' |head -1|sed -e 's,^ *Commit: *,,')
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
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac
