#!/usr/bin/bash
set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/../../ci/libpaprci/libbuild.sh

pkg_upgrade
pkg_install git rsync openssh-clients ansible standard-test-roles

# "Hot patch" this to pick up https://pagure.io/standard-test-roles/pull-request/152
# so we get parallelism
cd /usr/share/ansible/inventory && curl -L -O https://pagure.io/standard-test-roles/raw/master/f/inventory/standard-inventory-qcow2
