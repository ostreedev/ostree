/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 */

#pragma once

#include "ostree-bootloader.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_BOOTLOADER_GRUB2 (_ostree_bootloader_grub2_get_type ())
#define OSTREE_BOOTLOADER_GRUB2(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_BOOTLOADER_GRUB2, OstreeBootloaderGrub2))
#define OSTREE_IS_BOOTLOADER_GRUB2(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_BOOTLOADER_GRUB2))

typedef struct _OstreeBootloaderGrub2 OstreeBootloaderGrub2;

GType _ostree_bootloader_grub2_get_type (void) G_GNUC_CONST;

OstreeBootloaderGrub2 * _ostree_bootloader_grub2_new (OstreeSysroot *sysroot);

gboolean _ostree_bootloader_grub2_generate_config (OstreeSysroot *sysroot, int bootversion, int target_fd, GCancellable *cancellable, GError **error);

G_END_DECLS
