#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Set up ostree directory
#
# Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e
set -x

SRCDIR=`dirname $0`
WORKDIR=`pwd`

if test $(id -u) != 0; then
    cat <<EOF
This script should be run as root.
EOF
    exit 1
fi

usage () {
    echo "$0 OSTREE_DIR_PATH"
    exit 1
}

OSTREE_DIR_PATH=$1
shift
test -n "$OSTREE_DIR_PATH" || usage

cd "$OSTREE_DIR_PATH"
mkdir -p -m 0755 ./var/{log,run,tmp,spool}
mkdir -p ./var/lib/dbus
dbus-uuidgen > ./var/lib/dbus/machine-id

mkdir -p ./var/tmp
chmod 1777 ./var/tmp

mkdir ./var/lib/gdm
chown 2:2 ./var/lib/gdm

touch ./var/shadow
chmod 0600 ./var/shadow

cat >./var/passwd << EOF
root::0:0:root:/:/bin/sh
dbus:*:1:1:dbus:/:/bin/false
gdm:*:2:2:gdm:/var/lib/gdm:/bin/false
EOF
cat >./var/group << EOF
root:*:0:root
dbus:*:1:
gdm:*:2:
EOF

mkdir repo
ostree --repo=repo init
