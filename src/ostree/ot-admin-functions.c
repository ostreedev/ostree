/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-functions.h"
#include "otutil.h"
#include "ostree.h"
#include "libgsystem.h"

gboolean
ot_admin_require_booted_deployment_or_osname (OstreeSysroot       *sysroot,
                                              const char          *osname,
                                              GCancellable        *cancellable,
                                              GError             **error)
{
  gboolean ret = FALSE;
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (sysroot);

  if (booted_deployment == NULL && osname == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system and no --os= argument given");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ot_admin_checksum_version:
 * @checksum: A GVariant from an ostree checksum.
 *
 *
 * Get the version metadata string from a commit variant object, if it exists.
 *
 * Returns: A newly allocated string of the version, or %NULL is none
 */
char *
ot_admin_checksum_version (GVariant *checksum)
{
  gs_unref_variant GVariant *metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

OstreeDeployment *
ot_admin_get_indexed_deployment (OstreeSysroot  *sysroot,
                                 int             index,
                                 GError        **error)

{
  gs_unref_ptrarray GPtrArray *current_deployments =
    ostree_sysroot_get_deployments (sysroot);

  if (index < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Invalid index %d", index);
      return NULL;
    }
  if (index >= current_deployments->len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Out of range deployment index %d, expected < %d", index,
                   current_deployments->len);
      return NULL;
    }
  
  return g_object_ref (current_deployments->pdata[index]);
}

