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

static gboolean opt_no_repo_prune;

static GOptionEntry options[] = {
  { "no-repo-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_repo_prune, "Only prune deployment checkouts; don't prune repository", NULL },
  { NULL }
};

static gboolean
list_deployments (GFile        *from_dir,
                  GPtrArray    *inout_deployments,
                  GCancellable *cancellable,
                  GError      **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;

  dir_enum = g_file_enumerate_children (from_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, error)) != NULL)
    {
      const char *name;
      ot_lobj GFile *child = NULL;
      ot_lobj GFile *possible_etc = NULL;
      ot_lobj GFile *possible_usr = NULL;

      name = g_file_info_get_name (file_info);

      if (g_str_has_suffix (name, "-etc"))
        goto next;
      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        goto next;

      child = g_file_get_child (from_dir, name);

      possible_etc = ot_gfile_get_child_strconcat (from_dir, name, "-etc", NULL);
      /* Bit of a hack... */
      possible_usr = g_file_get_child (child, "usr");

      if (g_file_query_exists (possible_etc, cancellable))
        g_ptr_array_add (inout_deployments, g_file_get_child (from_dir, name));
      else if (g_file_query_exists (possible_usr, cancellable))
        goto next;
      else
        {
          if (!list_deployments (child, inout_deployments,
                                 cancellable, error))
            goto out;
        }

    next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_builtin_prune (int argc, char **argv, GFile *ostree_dir, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  guint i;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *current_deployment = NULL;
  ot_lobj GFile *previous_deployment = NULL;
  ot_lptrarray GPtrArray *deployments = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("- Delete untagged deployments and repository objects");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ot_admin_ensure_initialized (ostree_dir, cancellable, error))
    goto out;

  deployments = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!list_deployments (ostree_dir, deployments, cancellable, error))
    goto out;

  if (!ot_admin_get_current_deployment (ostree_dir, &current_deployment,
                                        cancellable, error));
  if (!ot_admin_get_previous_deployment (ostree_dir, &previous_deployment,
                                         cancellable, error));

  for (i = 0; i < deployments->len; i++)
    {
      GFile *deployment = deployments->pdata[i];
      ot_lobj GFile *deployment_etc = NULL;
      ot_lobj GFile *parent = NULL;

      if ((current_deployment && g_file_equal (deployment, current_deployment))
          || (previous_deployment && g_file_equal (deployment, previous_deployment)))
        continue;

      parent = g_file_get_parent (deployment);
      deployment_etc = ot_gfile_get_child_strconcat (parent, gs_file_get_basename_cached (deployment),
                                                     "-etc", NULL);
      
      g_print ("Deleting deployment %s\n", gs_file_get_path_cached (deployment));
      if (!gs_shutil_rm_rf (deployment, cancellable, error))
        goto out;
      /* Note - not atomic; we may be leaving the -etc directory around
       * if this fails in the middle =/
       */
      if (!gs_shutil_rm_rf (deployment_etc, cancellable, error))
        goto out;
    }
  
  repo_path = g_file_get_child (ostree_dir, "repo");

  if (!opt_no_repo_prune)
    {
      ot_lfree char *repo_arg = NULL;

      repo_arg = g_strconcat ("--repo=", gs_file_get_path_cached (repo_path), NULL);
      
      if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                          GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                          cancellable, error,
                                          "ostree", repo_arg, "prune", "--refs-only",
                                          "--depth=0", NULL))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
