#!/usr/bin/bash
# Generate a src.rpm, then binary rpms in the current directory

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Auto-provision bootstrap resources if run as root (normally in CI)
if test "$(id -u)" == 0; then
    pkg_install_buildroot
    pkg_install make /usr/bin/rpmbuild git
fi

# PAPR really should do this
if ! test -f libglnx/README.md || ! test -f bsdiff/README.md; then
    git submodule update --init
fi

# Default libcurl on by default in fedora unless libsoup is enabled
if test "${OS_ID}" = 'fedora'; then
    case "${CONFIGOPTS:-}" in
        *--with-soup*|*--without-curl*) ;;
        *) CONFIGOPTS="${CONFIGOPTS:-} --with-curl"
    esac
fi
case "${CONFIGOPTS:-}" in
    *--with-curl*|*--with-soup*)
        if test -x /usr/bin/gnome-desktop-testing-runner; then
            CONFIGOPTS="${CONFIGOPTS} --enable-installed-tests=exclusive"
        fi
        ;;
esac

# TODO: Use some form of rpm's --build-in-place to skip archive-then-unpack?
make -f ${dn}/Makefile.dist-packaging srpm PACKAGE=libostree DISTGIT_NAME=ostree
if test "$(id -u)" == 0; then
    pkg_builddep *.src.rpm
else
    echo "NOTE: Running as non-root, assuming build dependencies are installed"
fi
if ! ${dn}/rpmbuild-cwd --rebuild *.src.rpm; then
    find . -type f -name config.log -exec cat {} \;
    exit 1
fi
