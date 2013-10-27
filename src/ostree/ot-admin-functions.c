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

gboolean
ot_admin_complete_deploy_one (OstreeSysroot      *sysroot,
                              const char         *osname,
                              OstreeDeployment   *new_deployment,
                              OstreeDeployment   *merge_deployment,
                              gboolean            opt_retain,
                              GCancellable       *cancellable,
                              GError            **error)
{
  gboolean ret = FALSE;
  guint i;
  OstreeDeployment *booted_deployment = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  deployments = ostree_sysroot_get_deployments (sysroot);
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  g_ptr_array_add (new_deployments, g_object_ref (new_deployment));

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      
      /* Keep deployments with different osnames, as well as the
       * booted and merge deployments
       */
      if (opt_retain ||
          (osname != NULL &&
           strcmp (ostree_deployment_get_osname (deployment), osname) != 0) ||
          ostree_deployment_equal (deployment, booted_deployment) ||
          ostree_deployment_equal (deployment, merge_deployment))
        {
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
        }
      else
        {
          g_print ("ostadmin: Will delete deployment osname=%s %s.%u\n",
                   ostree_deployment_get_osname (deployment),
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
        }
    }

  if (!ostree_sysroot_write_deployments (sysroot, new_deployments, cancellable, error))
    goto out;

  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
