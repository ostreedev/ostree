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

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "libgsystem.h"

#include <glib/gi18n.h>

static char *opt_osname;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Specify operating system root to use", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_diff (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object GFile *repo_path = NULL;
  gs_unref_object OstreeDeployment *deployment = NULL;
  gs_unref_object GFile *deployment_dir = NULL;
  gs_unref_ptrarray GPtrArray *modified = NULL;
  gs_unref_ptrarray GPtrArray *removed = NULL;
  gs_unref_ptrarray GPtrArray *added = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_object GFile *orig_etc_path = NULL;
  gs_unref_object GFile *new_etc_path = NULL;
  int bootversion;

  context = g_option_context_new ("Diff current /etc configuration versus default");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;
  
  repo_path = g_file_resolve_relative_path (ostree_sysroot_get_path (sysroot), "ostree/repo");

  if (!ostree_sysroot_list_deployments (sysroot, &bootversion, &deployments,
                                        cancellable, error))
    {
      g_prefix_error (error, "While listing deployments: ");
      goto out;
    }

  if (!ot_admin_require_deployment_or_osname (ostree_sysroot_get_path (sysroot), deployments,
                                              opt_osname, &deployment,
                                              cancellable, error))
    goto out;
  if (deployment != NULL)
    opt_osname = (char*)ostree_deployment_get_osname (deployment);
  if (deployment == NULL)
    deployment = ot_admin_get_merge_deployment (deployments, opt_osname, deployment);
  if (deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No deployment for OS '%s'", opt_osname);
      goto out;
    }

  deployment_dir = ostree_sysroot_get_deployment_directory (sysroot, deployment);

  orig_etc_path = g_file_resolve_relative_path (deployment_dir, "usr/etc");
  new_etc_path = g_file_resolve_relative_path (deployment_dir, "etc");
  
  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (orig_etc_path, new_etc_path, modified, removed, added,
                         cancellable, error))
    goto out;

  ostree_diff_print (orig_etc_path, new_etc_path, modified, removed, added);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
