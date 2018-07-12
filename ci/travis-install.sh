#!/bin/bash

# Copyright © 2015-2016 Collabora Ltd.
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -euo pipefail
set -x

NULL=

# ci_distro:
# OS distribution in which we are testing
# Typical values: ubuntu, debian; maybe fedora in future
: "${ci_distro:=debian}"

# ci_docker:
# If non-empty, this is the name of a Docker image. travis-install.sh will
# fetch it with "docker pull" and use it as a base for a new Docker image
# named "ci-image" in which we will do our testing.
: "${ci_docker:=}"

# ci_in_docker:
# Used internally by travis-install.sh. If yes, we are inside the Docker image
# (ci_docker is empty in this case).
: "${ci_in_docker:=no}"

# ci_suite:
# OS suite (release, branch) in which we are testing.
# Typical values for ci_distro=ubuntu: xenial, trusty
# Typical values for ci_distro=debian: sid, jessie
# Typical values for ci_distro=fedora might be 25, rawhide
: "${ci_suite:=stretch}"

# ci_configopts: Additional arguments for configure
: "${ci_configopts:=}"

# ci_pkgs: Additional packages to be installed
: "${ci_pkgs:=}"

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
        -e "s/@ci_pkgs@/${ci_pkgs}/" \
        < ci/travis-Dockerfile.in > Dockerfile
    exec docker build -t ci-image .
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
            libcurl4-openssl-dev \
            procps \
            python-gi \
            python-xattr \
            zlib1g-dev \
            python3-yaml \
            ${ci_pkgs:-} \
            ${NULL}

        if [ "$ci_in_docker" = yes ]; then
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
