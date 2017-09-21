/*
 * Copyright (C) 2013 Collabora Ltd
 *
 * Based on ot-bootloader-syslinux.h by Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
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
