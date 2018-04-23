#!/usr/bin/bash

dn=$(cd $(dirname $0) && pwd)

OS_ID=$(. /etc/os-release; echo $ID)
OS_VERSION_ID=$(. /etc/os-release; echo $VERSION_ID)

pkg_upgrade() {
    yum -y distro-sync
}

make() {
    /usr/bin/make -j $(getconf _NPROCESSORS_ONLN) "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --sysconfdir=/etc --prefix=/usr --libdir=/usr/lib64 "$@"
    make V=1
}

pkg_install() {
    yum -y install "$@"
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
        fedora) pkg_install dnf-plugins-core @buildsys-build;;
        centos) pkg_install yum-utils
                # Base buildroot, copied from the mock config sadly
                pkg_install bash bzip2 coreutils cpio diffutils system-release findutils gawk gcc gcc-c++ \
                            grep gzip info make patch redhat-rpm-config rpm-build sed shadow-utils tar \
                            unzip util-linux which xz;;
        *) fatal "pkg_install_buildroot(): Unhandled OS ${OS_ID}";;
    esac
}

pkg_builddep() {
    # This is sadly the only case where it's a different command
    if test -x /usr/bin/dnf; then
        dnf builddep -y "$@"

        # XXX: tmp hack -- see
        # https://github.com/ostreedev/ostree/pull/1539
        if rpm -q gpgme | grep -q gpgme-1.9.0-6.fc27; then
            dnf install -y https://kojipkgs.fedoraproject.org//packages/gpgme/1.10.0/4.fc27/x86_64/{gpgme{,-devel},python{2,3}-gpg}-1.10.0-4.fc27.x86_64.rpm
        fi
        # https://bugzilla.redhat.com/show_bug.cgi?id=1542453
        if rpm -q libgcrypt | grep -q libgcrypt-1.8.2-1.fc27; then
            dnf install -y https://kojipkgs.fedoraproject.org//packages/libgcrypt/1.8.2/2.fc27/x86_64/libgcrypt-1.8.2-2.fc27.x86_64.rpm
        fi
    else
        yum-builddep -y "$@"
    fi
}

# Install both build and runtime dependencies for $pkg
pkg_builddep_runtimedep() {
    local pkg=$1
    pkg_builddep $pkg
    pkg_install $pkg
    rpm -e $pkg
}
