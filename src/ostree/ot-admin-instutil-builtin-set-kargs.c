/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>

#include "ot-admin-instutil-builtins.h"

#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_instutil_builtin_set_kargs (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  guint i;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  OstreeDeployment *first_deployment = NULL;
  GOptionContext *context = NULL;
  gs_unref_ptrarray GPtrArray *new_kargs = NULL;

  context = g_option_context_new ("ARGS - set new kernel command line arguments");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find a deployment in sysroot");
      goto out;
    }
  first_deployment = deployments->pdata[0];

  new_kargs = g_ptr_array_new ();
  for (i = 1; i < argc; i++)
    g_ptr_array_add (new_kargs, argv[i]);
  g_ptr_array_add (new_kargs, NULL);
  
  if (!ostree_sysroot_deployment_set_kargs (sysroot, first_deployment,
                                            (char**)new_kargs->pdata,
                                            cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
