/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#ifndef __OT_BOOTLOADER_H__
#define __OT_BOOTLOADER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define OT_TYPE_BOOTLOADER (ot_bootloader_get_type ())
#define OT_BOOTLOADER(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OT_TYPE_BOOTLOADER, OtBootloader))
#define OT_IS_BOOTLOADER(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OT_TYPE_BOOTLOADER))
#define OT_BOOTLOADER_GET_IFACE(inst) (G_TYPE_INSTANCE_GET_INTERFACE ((inst), OT_TYPE_BOOTLOADER, OtBootloaderInterface))

typedef struct _OtBootloader OtBootloader;
typedef struct _OtBootloaderInterface                            OtBootloaderInterface;

struct _OtBootloaderInterface
{
  GTypeInterface g_iface;

  /* virtual functions */
  gboolean             (* query)                  (OtBootloader  *self);
  gboolean             (* write_config)           (OtBootloader  *self,
                                                   int            bootversion,
                                                   GCancellable  *cancellable,
                                                   GError       **error);
};

GType ot_bootloader_get_type (void) G_GNUC_CONST;

gboolean ot_bootloader_query (OtBootloader *self);

gboolean ot_bootloader_write_config (OtBootloader  *self,
                                     int            bootversion,
                                     GCancellable  *cancellable,
                                     GError       **error);

G_END_DECLS

#endif /* __OT_BOOTLOADER_H__ */
