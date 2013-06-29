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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_status (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  int bootversion;
  gs_unref_object OtDeployment *booted_deployment = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;
  guint i;

  context = g_option_context_new ("List deployments");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ot_admin_list_deployments (admin_opts->sysroot, &bootversion, &deployments,
                                  cancellable, error))
    {
      g_prefix_error (error, "While listing deployments: ");
      goto out;
    }

  /* Find the currently booted deployment, if any; we will
   * ensure it is present in the new deployment list.
   */
  if (!ot_admin_find_booted_deployment (admin_opts->sysroot, deployments,
                                        &booted_deployment,
                                        cancellable, error))
    goto out;

  if (deployments->len == 0)
    {
      g_print ("No deployments.\n");
    }
  else
    {
      int subbootversion;

      if (!ot_admin_read_current_subbootversion (admin_opts->sysroot, bootversion,
                                                 &subbootversion,
                                                 cancellable, error))
        goto out;

      g_print ("bootversion: %d.%d\n", bootversion, subbootversion);

      for (i = 0; i < deployments->len; i++)
        {
          OtDeployment *deployment = deployments->pdata[i];
          g_print ("%u: %c %s %s.%d\n",
                   i,
                   deployment == booted_deployment ? '*' : ' ',
                   ot_deployment_get_osname (deployment),
                   ot_deployment_get_csum (deployment),
                   ot_deployment_get_deployserial (deployment));
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
