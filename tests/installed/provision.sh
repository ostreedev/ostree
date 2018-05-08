#!/usr/bin/bash
set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/../../ci/libpaprci/libbuild.sh

pkg_upgrade
pkg_install git rsync openssh-clients ansible standard-test-roles
