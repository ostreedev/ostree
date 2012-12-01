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

static gboolean opt_no_kernel;

static GOptionEntry options[] = {
  { "no-kernel", 0, 0, G_OPTION_ARG_NONE, &opt_no_kernel, "Don't update kernel related config (initramfs, bootloader)", NULL },
  { NULL }
};

static char *
parse_deploy_name_from_path (GFile   *ostree_dir,
                             GFile   *path)
{
  ot_lobj GFile *deploy_dir = g_file_get_child (ostree_dir, "deploy");
  ot_lfree char *relpath = g_file_get_relative_path (deploy_dir, path);
  const char *last_dash;

  last_dash = strrchr (relpath, '-');
  if (!last_dash)
    g_error ("Failed to parse deployment name %s", relpath);
  
  return g_strndup (relpath, last_dash - relpath);
}

static char *
remote_name_from_path (GKeyFile    *repo_config,
                       const char  *deploy_path)
{
  const char *group_prefix = "remote \"";
  char **groups = NULL;
  char **group_iter = NULL;
  char *ret = NULL;

  groups = g_key_file_get_groups (repo_config, NULL);
  for (group_iter = groups; *group_iter; group_iter++)
    {
      const char *group = *group_iter;
      char **configured_branches = NULL;
      char **branch_iter = NULL;
      gboolean found = FALSE;

      if (!(g_str_has_prefix (group, group_prefix)
            && g_str_has_suffix (group, "\"")))
        continue;

      configured_branches = g_key_file_get_string_list (repo_config, group, "branches", NULL, NULL);
      if (!configured_branches)
        continue;

      for (branch_iter = configured_branches; *branch_iter; branch_iter++)
        {
          const char *branch = *branch_iter;

          if (!strcmp (branch, deploy_path))
            {
              found = TRUE;
              break;
            }
        }
      
      if (found)
        break;
    }

  if (*group_iter)
    {
      const char *group = *group_iter;
      size_t len;
      ret = g_strdup (group + strlen (group_prefix));
      len = strlen (ret);
      g_assert (len > 0 && ret[len-1] == '\"');
      ret[len-1] = '\0';
    }
  g_strfreev (groups);
  return ret;
}

gboolean
ot_admin_builtin_pull_deploy (int argc, char **argv, GFile *ostree_dir, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lobj GFile *current_deployment = NULL;
  ot_lfree char *deploy_name = NULL;
  ot_lfree char *remote_name = NULL;
  ot_lptrarray GPtrArray *subproc_args = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new (" - Upgrade and redeploy current tree");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ot_admin_get_current_deployment (ostree_dir, &current_deployment,
                                        cancellable, error))
    goto out;

  if (!current_deployment)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No current deployment");
      goto out;
    }

  deploy_name = parse_deploy_name_from_path (ostree_dir, current_deployment);

  repo_path = g_file_get_child (ostree_dir, "repo");
  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  remote_name = remote_name_from_path (ostree_repo_get_config (repo),
                                       deploy_name);

  subproc_args = g_ptr_array_new ();
  {
    ot_lfree char *repo_arg = g_strconcat ("--repo=",
                                           gs_file_get_path_cached (repo_path),
                                           NULL);
    ot_ptrarray_add_many (subproc_args, "ostree", "pull", repo_arg, remote_name, NULL);
    g_ptr_array_add (subproc_args, NULL);

    if (!ot_spawn_sync_checked (gs_file_get_path_cached (ostree_dir),
                                (char**)subproc_args->pdata,
                                cancellable, error))
      goto out;
    
    g_clear_pointer (&subproc_args, (GDestroyNotify)g_ptr_array_unref);
  }

  subproc_args = g_ptr_array_new ();
  {
    ot_lfree char *opt_ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                                     gs_file_get_path_cached (ostree_dir),
                                                     NULL);
    ot_ptrarray_add_many (subproc_args, "ostree", "admin", opt_ostree_dir_arg, "deploy",
                          deploy_name, NULL);
    g_ptr_array_add (subproc_args, NULL);

    if (!ot_spawn_sync_checked (gs_file_get_path_cached (ostree_dir),
                                (char**)subproc_args->pdata,
                                cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
