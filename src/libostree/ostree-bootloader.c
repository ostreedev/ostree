/*
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
#include "ostree-bootloader.h"

G_DEFINE_INTERFACE (OstreeBootloader, _ostree_bootloader, G_TYPE_OBJECT)

static void
_ostree_bootloader_default_init (OstreeBootloaderInterface *iface)
{
}

gboolean
_ostree_bootloader_query (OstreeBootloader *self,
                          gboolean         *out_is_active,
                          GCancellable     *cancellable,
                          GError          **error)
{
  g_return_val_if_fail (OSTREE_IS_BOOTLOADER (self), FALSE);

  return OSTREE_BOOTLOADER_GET_IFACE (self)->query (self, out_is_active, cancellable, error);
}

/**
 * _ostree_bootloader_get_name:
 *
 * Returns: (transfer none): Name of this bootloader
 */
const char *
_ostree_bootloader_get_name (OstreeBootloader  *self)
{
  g_return_val_if_fail (OSTREE_IS_BOOTLOADER (self), NULL);

  return OSTREE_BOOTLOADER_GET_IFACE (self)->get_name (self);
}

gboolean
_ostree_bootloader_write_config (OstreeBootloader  *self,
                            int            bootversion,
                            GCancellable  *cancellable,
                            GError       **error)
{
  g_return_val_if_fail (OSTREE_IS_BOOTLOADER (self), FALSE);

  return OSTREE_BOOTLOADER_GET_IFACE (self)->write_config (self, bootversion, 
                                                       cancellable, error);
}

gboolean
_ostree_bootloader_is_atomic (OstreeBootloader  *self)
{
  g_return_val_if_fail (OSTREE_IS_BOOTLOADER (self), FALSE);

  if (OSTREE_BOOTLOADER_GET_IFACE (self)->is_atomic)
    return OSTREE_BOOTLOADER_GET_IFACE (self)->is_atomic (self);
  else
    return TRUE;
}
