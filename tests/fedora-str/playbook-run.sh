#!/usr/bin/bash
# A thin wrapper for ansible-playbook which has a nice check for
# TEST_SUBJECTS being set.
set -xeuo pipefail

# https://fedoraproject.org/wiki/CI/Tests
if test -z "${TEST_SUBJECTS:-}"; then
    cat <<EOF

error: TEST_SUBJECTS must be set; e.g.:

  curl -Lo fedora-atomic-host.qcow2 'https://getfedora.org/atomic_qcow2_latest'
  export TEST_SUBJECTS=\$(pwd)/fedora-atomic-host.qcow2

If you're doing interactive development, we recommend caching the qcow2
somewhere persistent.
EOF
    exit 1
fi
ls -al ${TEST_SUBJECTS}

export ANSIBLE_INVENTORY=${ANSIBLE_INVENTORY:-$(test -e inventory && echo inventory || echo /usr/share/ansible/inventory)}
ls -al /dev/kvm
exec ansible-playbook --tags=atomic "$@"
