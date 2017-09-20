#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_upgrade
pkg_install_builddeps ostree
# Until this propagates farther
pkg_install 'pkgconfig(libcurl)' 'pkgconfig(openssl)'
pkg_install sudo which attr fuse \
    libubsan libasan libtsan PyYAML redhat-rpm-config \
    elfutils
if test -n "${CI_PKGS:-}"; then
    pkg_install ${CI_PKGS}
fi
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang

# always fail on warnings; https://github.com/ostreedev/ostree/pull/971
export CFLAGS="-Werror ${CFLAGS:-}"

# Default libcurl on by default in fedora unless libsoup is enabled
if sh -c '. /etc/os-release; test "${ID}" = fedora'; then
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

build --enable-gtk-doc ${CONFIGOPTS:-}
