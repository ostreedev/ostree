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

NULL=

# Get the OS release info
. /etc/os-release

case "$ID" in
    (debian|ubuntu)
        # Make debconf run non-interactively since its questions can't
        # be answered.
        export DEBIAN_FRONTEND=noninteractive

        # TODO: fetch this list from the Debian packaging git repository?
        apt-get -y update
        apt-get -y install \
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
            libcurl4-openssl-dev \
            libfuse-dev \
            libgirepository1.0-dev \
            libglib2.0-dev \
            libgpgme11-dev \
            liblzma-dev \
            libmount-dev \
            libselinux1-dev \
            libsoup2.4-dev \
            libsystemd-dev \
            procps \
            python3-yaml \
            systemd \
            zlib1g-dev \
            "$@"
        ;;

    (*)
        echo "Don't know how to set up ${ID}" >&2
        exit 1
        ;;
esac

# vim:set sw=4 sts=4 et:
