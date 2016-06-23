#!/bin/bash

set -euo pipefail
set -x

NULL=
: "${ci_docker:=}"
: "${ci_in_docker:=}"
: "${ci_suite:=jessie}"

if [ $(id -u) = 0 ]; then
    sudo=
else
    sudo=sudo
fi

if [ -n "$ci_docker" ]; then
    sed \
        -e "s/@ci_distro@/${ci_distro}/" \
        -e "s/@ci_docker@/${ci_docker}/" \
        -e "s/@ci_suite@/${ci_suite}/" \
        < tests/ci-Dockerfile.in > Dockerfile
    exec docker build -t ostree-ci .
fi

case "$ci_distro" in
    (debian)
        # Docker images use httpredir.debian.org but it seems to be
        # unreliable; use a CDN instead
        sed -i -e 's/httpredir\.debian\.org/deb.debian.org/g' /etc/apt/sources.list
        ;;
esac

case "$ci_distro" in
    (debian|ubuntu)
        # TODO: fetch this list from the Debian packaging git repository?
        $sudo apt-get -y update
        $sudo apt-get -y install \
            attr \
            bison \
            cpio \
            debhelper \
            dh-autoreconf \
            dh-systemd \
            docbook-xml \
            docbook-xsl \
            e2fslibs-dev \
            elfutils \
            fuse \
            gjs \
            gnome-desktop-testing \
            gobject-introspection \
            gtk-doc-tools \
            libarchive-dev \
            libattr1-dev \
            libcap-dev \
            libfuse-dev \
            libgirepository1.0-dev \
            libglib2.0-dev \
            libgpgme11-dev \
            liblzma-dev \
            libmount-dev \
            libselinux1-dev \
            libsoup2.4-dev \
            procps \
            zlib1g-dev \
            ${NULL}

        if [ -n "$ci_in_docker" ]; then
            # Add the user that we will use to do the build inside the
            # Docker container, and let them use sudo
            adduser --disabled-password user </dev/null
            apt-get -y install sudo systemd-sysv
            echo "user ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/nopasswd
            chmod 0440 /etc/sudoers.d/nopasswd
        fi
        ;;

    (*)
        echo "Don't know how to set up ${ci_distro}" >&2
        exit 1
        ;;
esac

# vim:set sw=4 sts=4 et:
