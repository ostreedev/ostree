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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_undeploy (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  const char *deploy_index_str;
  int deploy_index;
  g_autoptr(GPtrArray) current_deployments = NULL;
  glnx_unref_object OstreeDeployment *target_deployment = NULL;

  context = g_option_context_new ("INDEX - Delete deployment INDEX");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "INDEX must be specified", error);
      goto out;
    }

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;
  current_deployments = ostree_sysroot_get_deployments (sysroot);

  deploy_index_str = argv[1];
  deploy_index = atoi (deploy_index_str);

  target_deployment = ot_admin_get_indexed_deployment (sysroot, deploy_index, error);
  if (!target_deployment)
    goto out;

  if (target_deployment == ostree_sysroot_get_booted_deployment (sysroot))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot undeploy currently booted deployment %i", deploy_index);
      goto out;
    }
  
  g_ptr_array_remove_index (current_deployments, deploy_index);

  if (!ostree_sysroot_write_deployments (sysroot, current_deployments,
                                         cancellable, error))
    goto out;

  g_print ("Deleted deployment %s.%d\n", ostree_deployment_get_csum (target_deployment),
           ostree_deployment_get_deployserial (target_deployment));
  
  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
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
