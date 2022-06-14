/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
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
ot_admin_builtin_undeploy (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("INDEX");

  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "INDEX must be specified", error);
      return FALSE;
    }

  g_autoptr(GPtrArray) current_deployments = ostree_sysroot_get_deployments (sysroot);

  const char *deploy_index_str = argv[1];
  int deploy_index = atoi (deploy_index_str);

  g_autoptr(OstreeDeployment) target_deployment =
    ot_admin_get_indexed_deployment (sysroot, deploy_index, error);
  if (!target_deployment)
    return FALSE;

  if (target_deployment == ostree_sysroot_get_booted_deployment (sysroot))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot undeploy currently booted deployment %i", deploy_index);
      return FALSE;
    }

  g_ptr_array_remove_index (current_deployments, deploy_index);

  if (!ostree_sysroot_write_deployments (sysroot, current_deployments,
                                         cancellable, error))
    return FALSE;

  g_print ("Deleted deployment %s.%d\n", ostree_deployment_get_csum (target_deployment),
           ostree_deployment_get_deployserial (target_deployment));

  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    return glnx_prefix_error (error, "Performing final cleanup");

  return TRUE;
}
