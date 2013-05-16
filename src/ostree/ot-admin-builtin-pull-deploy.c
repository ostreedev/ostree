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

static gboolean
ensure_remote_branch (OstreeRepo    *repo,
                      const char    *remote,
                      const char    *branch,
                      GCancellable  *cancellable,
                      GError       **error)
{
  gboolean ret = FALSE;
  gchar **iter = NULL;
  gsize len;
  gs_free char *remote_key = NULL;
  gs_unref_ptrarray GPtrArray *new_branches = NULL;
  GKeyFile *config = NULL;
  gchar **branches = NULL;
  gboolean have_branch = FALSE;
  
  config = ostree_repo_copy_config (repo);
  remote_key = g_strdup_printf ("remote \"%s\"", remote);

  new_branches = g_ptr_array_new ();

  branches = g_key_file_get_string_list (config, remote_key, "branches", &len, error);
  if (!branches)
    goto out;

  for (iter = branches; *iter; iter++)
    {
      char *item = *iter;
      if (!have_branch)
        have_branch = strcmp (item, branch) == 0;
      g_ptr_array_add (new_branches, item);
    }

  if (!have_branch)
    {
      g_ptr_array_add (new_branches, (char*)branch);
      g_key_file_set_string_list (config, remote_key, "branches",
                                  (const char *const *)new_branches->pdata,
                                  new_branches->len);
      
      if (!ostree_repo_write_config (repo, config, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (config)
    g_key_file_free (config);
  if (branches)
    g_strfreev (branches);
  return ret;
}

gboolean
ot_admin_builtin_pull_deploy (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *osname;
  const char *target;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *current_deployment = NULL;
  ot_lfree char *deploy_name = NULL;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lfree char *remote_name = NULL;
  ot_lptrarray GPtrArray *subproc_args = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME [TREE] - Ensure TREE (default current) is in list of remotes, then download and deploy");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  repo_path = g_file_get_child (ostree_dir, "repo");

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc > 2)
    {
      target = argv[2];
      if (!ensure_remote_branch (repo, osname, target,
                                cancellable, error))
        goto out;
      
      deploy_name = g_strdup (target);
    }
  else
    {
      if (!ot_admin_get_current_deployment (ostree_dir, osname, &current_deployment,
                                            cancellable, error))
        goto out;
      
      if (!current_deployment)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No current deployment");
          goto out;
        }
      
      ot_admin_parse_deploy_name (ostree_dir, osname, current_deployment,
                                  &deploy_name, NULL);
    }

  if (!ot_admin_pull (ostree_dir, osname, cancellable, error))
    goto out;

  {
    ot_lfree char *opt_ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                                     gs_file_get_path_cached (ostree_dir),
                                                     NULL);
    ot_lfree char *opt_boot_dir_arg = g_strconcat ("--boot-dir=",
                                                     gs_file_get_path_cached (admin_opts->boot_dir),
                                                     NULL);
    if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                        GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                        cancellable, error,
                                        "ostree", "admin", opt_ostree_dir_arg, opt_boot_dir_arg, "deploy", osname,
                                        deploy_name, NULL))
      goto out;
  }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
