#!/usr/bin/bash
# A thin wrapper for ansible-playbook which has a nice check for
# TEST_SUBJECTS being set.
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
if ! test -d build; then
    mkdir -p build
    (cd build && ${dn}/../../ci/build-rpm.sh)
fi

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
for subj in ${TEST_SUBJECTS}; do
    ls -al ${subj} && file ${subj}
done

# This is required
rpm -q standard-test-roles

export ANSIBLE_INVENTORY=${ANSIBLE_INVENTORY:-$(test -e inventory && echo inventory || echo /usr/share/ansible/inventory)}
ls -al /dev/kvm
# Sadly having this on makes the reboot playbook break
export ANSIBLE_SSH_ARGS='-o ControlMaster=no'
exec ansible-playbook --tags=atomic "$@"
