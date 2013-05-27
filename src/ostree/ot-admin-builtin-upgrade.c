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
#include "otutil.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static gboolean opt_reboot;

static GOptionEntry options[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after a successful upgrade", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GFile *ostree_dir = admin_opts->ostree_dir;
  gs_free char *booted_osname = NULL;
  const char *osname = NULL;
  gs_unref_object GFile *deployment = NULL;
  gs_unref_object GFile *repo_path = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *deploy_name = NULL;
  gs_free char *current_rev = NULL;
  gs_free char *new_rev = NULL;
  gs_free char *ostree_dir_arg = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("[OSNAME] - pull, deploy, and prune");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    {
      osname = argv[1];
    }
  else
    {
      if (!ot_admin_get_booted_os (&booted_osname, NULL,
                                   cancellable, error))
        goto out;
      if (booted_osname == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Not in an active OSTree system; must specify OSNAME");
          goto out;
        }
      osname = booted_osname;
    }
  
  if (!ot_admin_get_current_deployment (ostree_dir, osname, &deployment,
                                        cancellable, error))
    goto out;

  ot_admin_parse_deploy_name (ostree_dir, osname, deployment, &deploy_name, &current_rev);

  ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                gs_file_get_path_cached (ostree_dir),
                                NULL);
  
  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "pull-deploy", osname, NULL))
    goto out;

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "prune", osname, NULL))
    goto out;

  if (opt_reboot)
    {
      repo_path = g_file_get_child (ostree_dir, "repo");

      repo = ostree_repo_new (repo_path);
      if (!ostree_repo_check (repo, error))
        goto out;

      if (!ostree_repo_resolve_rev (repo, deploy_name, TRUE, &new_rev,
                                    error))
        goto out;

      if (strcmp (current_rev, new_rev) != 0 && opt_reboot)
        {
          gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         cancellable, error,
                                         "systemctl", "reboot", NULL);
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
