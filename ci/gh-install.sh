#!/bin/bash

# Copyright © 2015-2016 Collabora Ltd.
# Copyright © 2021 Endless OS Foundation LLC
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

# Get the OS release info
. /etc/os-release

case "$ID" in
    (debian|ubuntu)
        # Make debconf run non-interactively since its questions can't
        # be answered.
        export DEBIAN_FRONTEND=noninteractive

        # Debian upstream data:
        # https://tracker.debian.org/pkg/ostree
        # https://salsa.debian.org/debian/ostree
        # https://salsa.debian.org/debian/ostree/-/blob/debian/master/debian/control
        #
        # Ubuntu package data:
        # https://packages.ubuntu.com/source/impish/ostree
        #
        # Use libfuse3-dev unless otherwise specified
        case " $* " in
            (*\ libfuse-dev\ *)
                ;;

            (*\ libfuse3-dev\ *)
                ;;

            (*)
                set -- "$@" libfuse3-dev
                ;;
        esac

        # TODO: fetch this list from the Debian packaging git repository?

        # First construct a list of Build-Depends common to all
        # versions. This includes build-essential, which is assumed to
        # be installed on all Debian builders. We also add gjs to allow
        # the JS tests to run even though gjs is explicitly disable in
        # Debian.
        PACKAGES=(
            attr
            autoconf
            automake
            bison
            build-essential
            bubblewrap
            ca-certificates
            cpio
            debhelper
            dh-exec
            docbook-xml
            docbook-xsl
            e2fslibs-dev
            elfutils
            fuse
            gnupg
            gobject-introspection
            gtk-doc-tools
            libarchive-dev
            libattr1-dev
            libavahi-client-dev
            libavahi-glib-dev
            libcap-dev
            libfuse-dev
            libgirepository1.0-dev
            libglib2.0-dev
            libglib2.0-doc
            libgpgme-dev
            liblzma-dev
            libmount-dev
            libselinux1-dev
            libsoup2.4-dev
            libsystemd-dev
            libtool
            libcap2-bin
            procps
            python3
            python3-yaml
            xsltproc
            zlib1g-dev
        )

        # Additional common packages:
        #
        # gjs - To allow running JS tests even though this has been
        # disabled in Debian for a while.
        #
        # gnome-desktop-testing - To eventually allow running the
        # installed tests.
        #
        # libcurl4-openssl-dev - To allow building the cURL fetch
        # backend in addition to the soup fetch backend.
        #
        # systemd - To get the unit and generator paths from systemd.pc
        # rather than passing them as configure options.
        PACKAGES+=(
            gjs
            gnome-desktop-testing
            libcurl4-openssl-dev
            systemd
        )

        # Distro specific packages. Matching is on VERSION_CODENAME from
        # /etc/os-release. Debian testing and unstable may not have this
        # set, so assume an empty or unset value represents those.

        # hexdump was previously provided by bsdmainutils but is now in
        # bsdextrautils.
        case "${VERSION_CODENAME:-}" in
            (buster|focal|bionic)
                PACKAGES+=(bsdmainutils)
                ;;
            (*)
                PACKAGES+=(bsdextrautils)
                ;;
        esac

        apt-get -y update
        apt-get -y install "${PACKAGES[@]}" "$@"
        ;;

    (*)
        echo "Don't know how to set up ${ID}" >&2
        exit 1
        ;;
esac

# vim:set sw=4 sts=4 et:
