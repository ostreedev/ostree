#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# https://bugzilla.redhat.com/show_bug.cgi?id=1483553
if ! yum -y upgrade 2>err.txt; then
    ecode=$?
    if grep -q -F -e "BDB1539 Build signature doesn't match environment" err.txt; then
        rpm --rebuilddb
        yum -y upgrade
    else
        cat err.txt
        exit ${ecode}
    fi
fi

pkg_install_builddeps ostree
# Until this propagates farther
pkg_install 'pkgconfig(libcurl)' 'pkgconfig(openssl)'
pkg_install sudo which attr fuse \
    libubsan libasan libtsan PyYAML redhat-rpm-config \
    elfutils
pkg_install_if_os fedora gjs gnome-desktop-testing parallel coccinelle clang

# always fail on warnings; https://github.com/ostreedev/ostree/pull/971
export CFLAGS="-Werror ${CFLAGS:-}"

DETECTED_CONFIGOPTS=
if test -x /usr/bin/gnome-desktop-testing-runner; then
    DETECTED_CONFIGOPTS="${DETECTED_CONFIGOPTS} --enable-installed-tests=exclusive"
fi
# Default libcurl on by default in fedora unless libsoup is enabled
if sh -c '. /etc/os-release; test "${ID}" = fedora'; then
    case "${CONFIGOPTS:-}" in
        *--with-soup*) ;;
        *) CONFIGOPTS="${CONFIGOPTS:-} --with-curl"
    esac
fi
build --enable-gtk-doc ${DETECTED_CONFIGOPTS} ${CONFIGOPTS:-}
