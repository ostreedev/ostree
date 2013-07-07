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
#include "ot-admin-deploy.h"
#include "ostree.h"
#include "otutil.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static gboolean opt_reboot;
static char *opt_osname;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Specify operating system root to use", NULL },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after a successful upgrade", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  gboolean ret = FALSE;
  __attribute__((unused)) GCancellable *cancellable = NULL;
  GOptionContext *context;
  GFile *sysroot = admin_opts->sysroot;
  gs_free char *booted_osname = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *repo_path = NULL;
  gs_free char *origin_refspec = NULL;
  gs_free char *origin_remote = NULL;
  gs_free char *origin_ref = NULL;
  gs_free char *new_revision = NULL;
  gs_unref_object GFile *deployment_path = NULL;
  gs_unref_object GFile *deployment_origin_path = NULL;
  gs_unref_object OtDeployment *booted_deployment = NULL;
  gs_unref_object OtDeployment *merge_deployment = NULL;
  gs_unref_ptrarray GPtrArray *current_deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = NULL;
  gs_unref_object OtDeployment *new_deployment = NULL;
  gs_free char *ostree_dir_arg = NULL;
  int current_bootversion;
  int new_bootversion;
  GKeyFile *origin;

  context = g_option_context_new ("Construct new tree from current origin and deploy it, if it changed");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ot_admin_list_deployments (admin_opts->sysroot, &current_bootversion,
                                  &current_deployments,
                                  cancellable, error))
    {
      g_prefix_error (error, "While listing deployments: ");
      goto out;
    }

  if (!ot_admin_require_deployment_or_osname (admin_opts->sysroot, current_deployments,
                                              opt_osname,
                                              &booted_deployment,
                                              cancellable, error))
    goto out;
  if (!opt_osname)
    opt_osname = (char*)ot_deployment_get_osname (booted_deployment);
  merge_deployment = ot_admin_get_merge_deployment (current_deployments, opt_osname,
                                                    booted_deployment);

  deployment_path = ot_admin_get_deployment_directory (admin_opts->sysroot, merge_deployment);
  deployment_origin_path = ot_admin_get_deployment_origin_path (deployment_path);

  repo_path = g_file_resolve_relative_path (admin_opts->sysroot, "ostree/repo");
  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  origin = ot_deployment_get_origin (merge_deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for current deployment");
      goto out;
    }
  origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
  if (!origin_refspec)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin/refspec in current deployment origin; cannot upgrade via ostree");
      goto out;
    }
  if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
    goto out;

  if (origin_remote)
    {
      g_print ("Fetching remote %s ref %s\n", origin_remote, origin_ref);
      if (!ot_admin_pull (admin_opts->sysroot, origin_remote, origin_ref,
                          cancellable, error))
        goto out;
    }

  if (!ostree_repo_resolve_rev (repo, origin_ref, FALSE, &new_revision,
                                error))
    goto out;

  if (strcmp (ot_deployment_get_csum (merge_deployment), new_revision) == 0)
    {
      g_print ("Refspec %s is unchanged\n", origin_refspec);
    }
  else
    {
      gs_unref_object GFile *real_sysroot = g_file_new_for_path ("/");
      if (!ot_admin_deploy (admin_opts->sysroot,
                            current_bootversion, current_deployments,
                            opt_osname, new_revision, origin,
                            NULL, FALSE,
                            booted_deployment, merge_deployment,
                            &new_deployment, &new_bootversion, &new_deployments,
                            cancellable, error))
        goto out;

      if (opt_reboot && g_file_equal (sysroot, real_sysroot))
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
