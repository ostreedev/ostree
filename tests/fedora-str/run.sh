#!/usr/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
cd ${dn}

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
ansible-playbook --tags=atomic tests.yml
