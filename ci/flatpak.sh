#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

FLATPAK_TAG=master

dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh

codedir=$(pwd)

# Build and install ostree
cd ${codedir}
ci/build.sh
make install

# Build flatpak
tmpd=$(mktemp -d)
cd ${tmpd}
git clone --recursive --depth=1 -b ${FLATPAK_TAG} https://github.com/flatpak/flatpak
cd ${tmpd}/flatpak
ci/build.sh
# We want to capture automake results from flatpak
cleanup() {
    mv test-suite.log ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
