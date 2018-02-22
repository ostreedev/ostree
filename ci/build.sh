#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh

pkg_upgrade
pkg_install_buildroot
pkg_builddep ostree
pkg_install sudo which attr fuse strace \
    libubsan libasan libtsan PyYAML redhat-rpm-config \
    elfutils
if test -n "${CI_PKGS:-}"; then
    pkg_install ${CI_PKGS}
fi
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang \
                  python3-PyYAML
if test "${OS_ID}" = "centos"; then
    rpm -Uvh https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
    pkg_install python34{,-PyYAML}
fi

# Default libcurl on by default in fedora unless libsoup is enabled
if test "${OS_ID}" = 'fedora'; then
    case "${CONFIGOPTS:-}" in
        *--with-soup*|*--without-curl*) ;;
        *) CONFIGOPTS="${CONFIGOPTS:-} --with-curl"
    esac
fi
case "${CONFIGOPTS:-}" in
    *--with-curl*|--with-soup*)
        if test -x /usr/bin/gnome-desktop-testing-runner; then
            CONFIGOPTS="${CONFIGOPTS} --enable-installed-tests=exclusive"
        fi
        ;;
esac

# always fail on warnings; https://github.com/ostreedev/ostree/pull/971
# NB: this disables the default set of flags from configure.ac
export CFLAGS="-Wall -Werror ${CFLAGS:-}"

build --enable-gtk-doc ${CONFIGOPTS:-}
