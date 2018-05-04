#!/usr/bin/bash
# Run all installed tests; see README.md in this directory for more
# information.
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)

# TODO: parallelize this
PLAYBOOKS=${PLAYBOOKS:-nondestructive.yml destructive-ansible.yml destructive-unit.yml}
for playbook in $PLAYBOOKS; do
    time ${dn}/playbook-run.sh -v ${dn}/${playbook}
done
