/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
