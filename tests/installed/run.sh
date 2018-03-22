#!/usr/bin/bash
# Run all installed tests; see README.md in this directory for more
# information.
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)

if ! test -d build; then
    mkdir -p build
    (cd build && ${dn}/../../ci/build-rpm.sh)
fi

# TODO: parallelize this
PLAYBOOKS=${PLAYBOOKS:-nondestructive.yml destructive.yml}
for playbook in $PLAYBOOKS; do
    time ${dn}/playbook-run.sh -v ${dn}/${playbook}
done
