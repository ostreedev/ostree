/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include <stdlib.h>

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ot-admin-deploy.h"
#include "ot-ordered-hash.h"
#include "ostree.h"
#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_undeploy (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *deploy_index_str;
  int deploy_index;
  int current_bootversion;
  gs_unref_ptrarray GPtrArray *current_deployments = NULL;
  gs_unref_object OstreeDeployment *booted_deployment = NULL;
  gs_unref_object OstreeDeployment *target_deployment = NULL;

  context = g_option_context_new ("INDEX - Delete deployment INDEX");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "INDEX must be specified", error);
      goto out;
    }

  deploy_index_str = argv[1];
  deploy_index = atoi (deploy_index_str);

  if (!ot_admin_list_deployments (ostree_sysroot_get_path (sysroot), &current_bootversion, &current_deployments,
                                  cancellable, error))
    {
      g_prefix_error (error, "While listing deployments: ");
      goto out;
    }

  if (!ot_admin_find_booted_deployment (ostree_sysroot_get_path (sysroot), current_deployments, &booted_deployment,
                                        cancellable, error))
    goto out;

  if (deploy_index < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Invalid index %d", deploy_index);
      goto out;
    }
  if (deploy_index >= current_deployments->len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Out of range index %d, expected < %d", deploy_index, current_deployments->len);
      goto out;
    }
  
  target_deployment = g_object_ref (current_deployments->pdata[deploy_index]);
  if (target_deployment == booted_deployment)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot undeploy currently booted deployment %i", deploy_index);
      goto out;
    }
  
  g_ptr_array_remove_index (current_deployments, deploy_index);

  if (!ot_admin_write_deployments (ostree_sysroot_get_path (sysroot), current_bootversion,
                                   current_bootversion ? 0 : 1, current_deployments,
                                   cancellable, error))
    goto out;

  g_print ("Deleted deployment %s.%d\n", ostree_deployment_get_csum (target_deployment),
           ostree_deployment_get_deployserial (target_deployment));
  
  if (!ot_admin_cleanup (ostree_sysroot_get_path (sysroot), cancellable, error))
    {
      g_prefix_error (error, "Performing final cleanup: ");
      goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
