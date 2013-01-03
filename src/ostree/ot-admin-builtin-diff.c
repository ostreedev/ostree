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

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_diff (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *osname;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *deployment = NULL;
  ot_lobj GFile *deploy_parent = NULL;
  ot_lptrarray GPtrArray *modified = NULL;
  ot_lptrarray GPtrArray *removed = NULL;
  ot_lptrarray GPtrArray *added = NULL;
  ot_lobj GFile *orig_etc_path = NULL;
  ot_lobj GFile *new_etc_path = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME [REVISION] - Diff configuration for OSNAME");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;
  
  repo_path = g_file_get_child (ostree_dir, "repo");

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  if (argc > 2)
    {
      deployment = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname, argv[2], NULL);
      if (!g_file_query_exists (deployment, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Deployment %s doesn't exist", gs_file_get_path_cached (deployment));
          goto out;
        }
    }
  else
    {
      if (!ot_admin_get_current_deployment (ostree_dir, osname, &deployment,
                                            cancellable, error))
        goto out;
    }

  orig_etc_path = g_file_resolve_relative_path (deployment, "etc");
  deploy_parent = g_file_get_parent (deployment);
  new_etc_path = ot_gfile_get_child_strconcat (deploy_parent,
                                               gs_file_get_basename_cached (deployment),
                                               "-etc", NULL);
  
  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (orig_etc_path, new_etc_path, modified, removed, added,
                         cancellable, error))
    goto out;

  ostree_diff_print (new_etc_path, modified, removed, added);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
