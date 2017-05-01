#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 "$@"
    make -j 8
}

codedir=$(pwd)

# Core prep
yum -y install dnf-plugins-core @buildsys-build 'dnf-command(builddep)'
# build+install ostree, and build deps for both, so that our
# make install overrides the ostree via rpm
dnf builddep -y ostree flatpak
yum -y install flatpak && rpm -e flatpak
# we use yaml below
yum -y install python3-PyYAML

# Now get flatpak's deps from rhci file
tmpd=$(mktemp -d)
cd ${tmpd}
# Frozen to a tag for now on general principle
git clone --recursive --depth=1 -b 0.9.3 https://github.com/flatpak/flatpak
cd flatpak
python3 -c 'import yaml; y = list(yaml.load_all(open(".redhat-ci.yml")))[0]; print("\0".join(y["packages"]))' | xargs -0 yum install -y
# back to ostree and build
cd ${codedir}
build
make install
cd ${tmpd}/flatpak
patch -p1 < ${codedir}/ci/*.patch
build
# We want to capture automake results from flatpak
cleanup() {
    mv test-suite.log ${codedir} || true
}
trap cleanup EXIT
make -j 8 check
