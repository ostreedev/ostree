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

#include "config.h"
#include "ot-bootloader.h"
#include "libgsystem.h"

G_DEFINE_INTERFACE (OtBootloader, ot_bootloader, G_TYPE_OBJECT)

static void
ot_bootloader_default_init (OtBootloaderInterface *iface)
{
}

gboolean
ot_bootloader_query (OtBootloader  *self)
{
  g_return_val_if_fail (OT_IS_BOOTLOADER (self), FALSE);

  return OT_BOOTLOADER_GET_IFACE (self)->query (self);
}

/**
 * ot_bootloader_get_name:
 *
 * Returns: (transfer none): Name of this bootloader
 */
const char *
ot_bootloader_get_name (OtBootloader  *self)
{
  g_return_val_if_fail (OT_IS_BOOTLOADER (self), NULL);

  return OT_BOOTLOADER_GET_IFACE (self)->get_name (self);
}

gboolean
ot_bootloader_write_config (OtBootloader  *self,
                            int            bootversion,
                            GCancellable  *cancellable,
                            GError       **error)
{
  g_return_val_if_fail (OT_IS_BOOTLOADER (self), FALSE);

  return OT_BOOTLOADER_GET_IFACE (self)->write_config (self, bootversion, 
                                                       cancellable, error);
}
