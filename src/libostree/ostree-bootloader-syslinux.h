/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "ostree-bootloader.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_BOOTLOADER_SYSLINUX (_ostree_bootloader_syslinux_get_type ())
#define OSTREE_BOOTLOADER_SYSLINUX(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_BOOTLOADER_SYSLINUX, OstreeBootloaderSyslinux))
#define OSTREE_IS_BOOTLOADER_SYSLINUX(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_BOOTLOADER_SYSLINUX))

typedef struct _OstreeBootloaderSyslinux OstreeBootloaderSyslinux;

GType _ostree_bootloader_syslinux_get_type (void) G_GNUC_CONST;

OstreeBootloaderSyslinux * _ostree_bootloader_syslinux_new (OstreeSysroot *sysroot);

G_END_DECLS
