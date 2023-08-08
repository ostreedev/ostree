#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

check() {
    if [[ -x $systemdutildir/systemd ]] && [[ -x /usr/lib/ostree/ostree-prepare-root ]]; then
       return 255
    fi

    return 1
}

depends() {
    return 0
}

install() {
    dracut_install /usr/lib/ostree/ostree-prepare-root
    for r in /usr/lib /etc; do
        if test -f "$r/ostree/prepare-root.conf"; then
            inst_simple "$r/ostree/prepare-root.conf"
        fi
    done
    if test -f "/etc/ostree/initramfs-root-binding.key"; then
        inst_simple "/etc/ostree/initramfs-root-binding.key"
    fi
    inst_simple "${systemdsystemunitdir}/ostree-prepare-root.service"
    mkdir -p "${initdir}${systemdsystemconfdir}/initrd-root-fs.target.wants"
    ln_r "${systemdsystemunitdir}/ostree-prepare-root.service" \
        "${systemdsystemconfdir}/initrd-root-fs.target.wants/ostree-prepare-root.service"
}
