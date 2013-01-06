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
parse_deploy_name_from_path (GFile   *osdir,
                             GFile   *path)
{
  ot_lfree char *relpath = g_file_get_relative_path (osdir, path);
  const char *last_dash;

  g_assert (relpath);
  last_dash = strrchr (relpath, '-');
  if (!last_dash)
    g_error ("Failed to parse deployment name %s", relpath);
  
  return g_strndup (relpath, last_dash - relpath);
}

gboolean
ot_admin_builtin_pull_deploy (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *osname;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *current_deployment = NULL;
  ot_lfree char *deploy_name = NULL;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lobj GFile *os_dir = NULL;
  ot_lfree char *remote_name = NULL;
  ot_lptrarray GPtrArray *subproc_args = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME - Upgrade and redeploy current tree");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  if (!ot_admin_get_current_deployment (ostree_dir, osname, &current_deployment,
                                        cancellable, error))
    goto out;

  if (!current_deployment)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No current deployment");
      goto out;
    }

  deploy_dir = g_file_get_child (ostree_dir, "deploy");
  os_dir = g_file_get_child (deploy_dir, osname);
  g_print ("%s\n%s\n", gs_file_get_path_cached (os_dir),
           gs_file_get_path_cached (current_deployment));
  deploy_name = parse_deploy_name_from_path (os_dir, current_deployment);

  repo_path = g_file_get_child (ostree_dir, "repo");

  {
    ot_lfree char *repo_arg = g_strconcat ("--repo=",
                                           gs_file_get_path_cached (repo_path),
                                           NULL);

    if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                        GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                        cancellable, error,
                                        "ostree", "pull", repo_arg, osname, NULL))
      goto out;
  }

  {
    ot_lfree char *opt_ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                                     gs_file_get_path_cached (ostree_dir),
                                                     NULL);
    ot_lfree char *opt_boot_dir_arg = g_strconcat ("--boot-dir=",
                                                     gs_file_get_path_cached (admin_opts->boot_dir),
                                                     NULL);
    if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                        GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
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
