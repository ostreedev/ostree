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

  if (osname == NULL && booted_deployment)
    osname = ostree_deployment_get_osname (booted_deployment);

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

gboolean
ot_admin_deploy_prepare (OstreeSysroot      *sysroot,
                         const char         *osname,
                         OstreeDeployment  **out_merge_deployment,
                         char              **out_origin_remote,
                         char              **out_origin_ref,
                         GKeyFile          **out_origin,
                         GCancellable        *cancellable,
                         GError             **error)
{
  gboolean ret = FALSE;
  gs_free char *origin_refspec = NULL;
  gs_free char *origin_remote = NULL;
  gs_free char *origin_ref = NULL;
  gs_unref_object GFile *deployment_path = NULL;
  gs_unref_object GFile *deployment_origin_path = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  GKeyFile *origin;

  if (!ot_admin_require_booted_deployment_or_osname (sysroot, osname,
                                                     cancellable, error))
    goto out;
  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, osname); 
  if (merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No previous deployment for OS '%s'", osname);
      goto out;
    }

  deployment_path = ostree_sysroot_get_deployment_directory (sysroot, merge_deployment);
  deployment_origin_path = ostree_sysroot_get_deployment_origin_path (deployment_path);

  origin = ostree_deployment_get_origin (merge_deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for current deployment");
      goto out;
    }
  origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
  if (!origin_refspec)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin/refspec in current deployment origin; cannot upgrade via ostree");
      goto out;
    }
  if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_merge_deployment, &merge_deployment);
  gs_transfer_out_value (out_origin_remote, &origin_remote);
  gs_transfer_out_value (out_origin_ref, &origin_ref);
  gs_transfer_out_value (out_origin, &origin);
 out:
  g_clear_pointer (&origin, g_key_file_unref);
  return ret;
}
