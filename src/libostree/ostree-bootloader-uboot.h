/*
 * Copyright (C) 2013 Collabora Ltd
 *
 * Based on ot-bootloader-syslinux.h by Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Javier Martinez Canillas <javier.martinez@collabora.co.uk>
 */

#pragma once

#include "ostree-bootloader.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_BOOTLOADER_UBOOT (_ostree_bootloader_uboot_get_type ())
#define OSTREE_BOOTLOADER_UBOOT(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_BOOTLOADER_UBOOT, OstreeBootloaderUboot))
#define OSTREE_IS_BOOTLOADER_UBOOT(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_BOOTLOADER_UBOOT))

typedef struct _OstreeBootloaderUboot OstreeBootloaderUboot;

GType _ostree_bootloader_uboot_get_type (void) G_GNUC_CONST;

OstreeBootloaderUboot * _ostree_bootloader_uboot_new (OstreeSysroot *sysroot);

G_END_DECLS
