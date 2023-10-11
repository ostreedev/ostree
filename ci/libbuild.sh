#!/usr/bin/bash

dn=$(cd $(dirname $0) && pwd)

OS_ID=$(. /etc/os-release; echo $ID)
OS_ID_LIKE=$(. /etc/os-release; echo $ID ${ID_LIKE:-})
OS_VERSION_ID=$(. /etc/os-release; echo $VERSION_ID)

pkg_upgrade() {
    dnf -y distro-sync
}

make() {
    /usr/bin/make -j ${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN)} "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --sysconfdir=/etc --prefix=/usr --libdir=/usr/lib64 "$@"
    make V=1
}

pkg_install() {
    dnf -y install "$@"
}

pkg_install_if_os() {
    local os=$1
    shift
    if test "${os}" = "${OS_ID}"; then
        pkg_install "$@"
    else
        echo "Skipping installation targeted for ${os} (current OS: ${OS_ID}): $@"
    fi
}

pkg_install_buildroot() {
    case "${OS_ID}" in
        fedora)
            pkg_install dnf-plugins-core @buildsys-build;;
        *) fatal "pkg_install_buildroot(): Unhandled OS ${OS_ID}";;
    esac
}

pkg_builddep() {
    dnf builddep -y "$@"
}

# Install both build and runtime dependencies for $pkg
pkg_builddep_runtimedep() {
    local pkg=$1
    pkg_builddep $pkg
    pkg_install $pkg
    rpm -e $pkg
}
