#!/bin/bash
# Build and run flatpak's unit tests using the just-built ostree for this PR.

set -xeuo pipefail

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 "$@"
    make -j 8
}

# build+install ostree
build
sudo make install
tmpd=$(mktemp -d)
cd ${tmpd}
# Frozen to a tag for now on general principle
git clone --recursive --depth=1 -b 0.9.3 https://github.com/flatpak/flatpak
cd flatpak
dnf builddep -y flatpak
build
make -j 8 check
