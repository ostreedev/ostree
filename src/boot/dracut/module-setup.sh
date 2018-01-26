#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

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
    inst_simple "${systemdsystemunitdir}/ostree-prepare-root.service"
    mkdir -p "${initdir}${systemdsystemconfdir}/initrd-switch-root.target.wants"
    ln_r "${systemdsystemunitdir}/ostree-prepare-root.service" \
        "${systemdsystemconfdir}/initrd-switch-root.target.wants/ostree-prepare-root.service"
}
