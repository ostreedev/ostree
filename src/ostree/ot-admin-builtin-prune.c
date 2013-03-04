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


gboolean
ot_admin_builtin_prune (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  guint i;
  const char *osname;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lobj GFile *current_deployment = NULL;
  ot_lobj GFile *previous_deployment = NULL;
  ot_lobj GFile *active_deployment = NULL;
  ot_lptrarray GPtrArray *deployments = NULL;
  gs_free char *active_osname = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME - Delete untagged deployments and repository objects");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  if (!ot_admin_list_deployments (ostree_dir, osname, &deployments,
                                  cancellable, error))
    goto out;

  if (!ot_admin_get_current_deployment (ostree_dir, osname, &current_deployment,
                                        cancellable, error));
  if (!ot_admin_get_previous_deployment (ostree_dir, osname, &previous_deployment,
                                         cancellable, error));
  if (!ot_admin_get_active_deployment (ostree_dir, &active_osname, &active_deployment,
                                       cancellable, error));

  for (i = 0; i < deployments->len; i++)
    {
      GFile *deployment = deployments->pdata[i];
      ot_lobj GFile *deployment_etc = NULL;
      ot_lobj GFile *parent = NULL;

      if ((current_deployment && g_file_equal (deployment, current_deployment))
          || (previous_deployment && g_file_equal (deployment, previous_deployment))
          || (active_deployment && g_file_equal (deployment, active_deployment)))
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
                                          GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
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
