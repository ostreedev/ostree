/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <gio/gio.h>
#include "otutil.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_BOOTLOADER (_ostree_bootloader_get_type ())
#define OSTREE_BOOTLOADER(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_BOOTLOADER, OstreeBootloader))
#define OSTREE_IS_BOOTLOADER(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_BOOTLOADER))
#define OSTREE_BOOTLOADER_GET_IFACE(inst) (G_TYPE_INSTANCE_GET_INTERFACE ((inst), OSTREE_TYPE_BOOTLOADER, OstreeBootloaderInterface))

typedef struct _OstreeBootloader OstreeBootloader;
typedef struct _OstreeBootloaderInterface                            OstreeBootloaderInterface;

struct _OstreeBootloaderInterface
{
  GTypeInterface g_iface;

  /* virtual functions */
  gboolean             (* query)                  (OstreeBootloader *bootloader,
                                                   gboolean         *out_is_active,
                                                   GCancellable     *cancellable,
                                                   GError          **error);
  const char *         (* get_name)               (OstreeBootloader  *self);
  gboolean             (* write_config)           (OstreeBootloader  *self,
                                                   int            bootversion,
                                                   GCancellable  *cancellable,
                                                   GError       **error);
  gboolean             (* is_atomic)              (OstreeBootloader  *self);
};
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeBootloader, g_object_unref)

GType _ostree_bootloader_get_type (void) G_GNUC_CONST;

gboolean _ostree_bootloader_query (OstreeBootloader *bootloader,
                                   gboolean         *out_is_active,
                                   GCancellable     *cancellable,
                                   GError          **error);

const char *_ostree_bootloader_get_name (OstreeBootloader  *self);

gboolean _ostree_bootloader_write_config (OstreeBootloader  *self,
                                          int            bootversion,
                                          GCancellable  *cancellable,
                                          GError       **error);

gboolean _ostree_bootloader_is_atomic (OstreeBootloader  *self);

G_END_DECLS
