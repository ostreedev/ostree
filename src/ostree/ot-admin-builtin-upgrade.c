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
#include "ot-builtins-common.h"
#include "ostree.h"
#include "otutil.h"
#include "libgsystem.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static char *opt_osname;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Specify operating system root to use", NULL },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after a successful upgrade", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *origin_remote = NULL;
  gs_free char *origin_ref = NULL;
  gs_free char *origin_refspec = NULL;
  gs_free char *new_revision = NULL;
  gs_unref_object GFile *deployment_path = NULL;
  gs_unref_object GFile *deployment_origin_path = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  GKeyFile *origin;

  context = g_option_context_new ("Construct new tree from current origin and deploy it, if it changed");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;


  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (!ot_admin_deploy_prepare (sysroot, opt_osname, &merge_deployment,
                                &origin_remote, &origin_ref,
                                &origin,
                                cancellable, error))
    goto out;

  if (origin_remote)
    {
      OstreeRepoPullFlags pullflags = 0;
      char *refs_to_fetch[] = { origin_ref, NULL };
      GSConsole *console;
      gs_unref_object OstreeAsyncProgress *progress = NULL;

      g_print ("Fetching remote %s ref %s\n", origin_remote, origin_ref);

      console = gs_console_get ();
      if (console)
        {
          gs_console_begin_status_line (console, "", NULL, NULL);
          progress = ostree_async_progress_new_and_connect (ot_common_pull_progress, console);
        }

      if (!ostree_repo_pull (repo, origin_remote, refs_to_fetch, pullflags, progress,
                             cancellable, error))
        goto out;
      
      origin_refspec = g_strconcat (origin_remote, ":", origin_ref, NULL);
    }
  else
    origin_refspec = g_strdup (origin_ref);


  if (!ostree_repo_resolve_rev (repo, origin_refspec, FALSE, &new_revision,
                                error))
    goto out;
  
  if (strcmp (ostree_deployment_get_csum (merge_deployment), new_revision) == 0)
    {
      g_print ("Refspec %s is unchanged\n", origin_refspec);
    }
  else
    {
      gs_unref_object GFile *real_sysroot = g_file_new_for_path ("/");

      if (!opt_allow_downgrade)
        {
          const char *old_revision = ostree_deployment_get_csum (merge_deployment);
          gs_unref_variant GVariant *old_commit = NULL;
          gs_unref_variant GVariant *new_commit = NULL;

          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         old_revision,
                                         &old_commit,
                                         error))
            goto out;
          
          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         new_revision, &new_commit,
                                         error))
            goto out;

          if (ostree_commit_get_timestamp (old_commit) > ostree_commit_get_timestamp (new_commit))
            {
              GDateTime *old_ts = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (old_commit));
              GDateTime *new_ts = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (new_commit));
              gs_free char *old_ts_str = NULL;
              gs_free char *new_ts_str = NULL;

              g_assert (old_ts);
              g_assert (new_ts);
              old_ts_str = g_date_time_format (old_ts, "%c");
              new_ts_str = g_date_time_format (new_ts, "%c");
              g_date_time_unref (old_ts);
              g_date_time_unref (new_ts);

              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Upgrade target revision '%s' with timestamp '%s' is chronologically older than current revision '%s' with timestamp '%s'; use --allow-downgrade to permit",
                           new_revision, new_ts_str, old_revision, old_ts_str);
              goto out;
            }
        }
      
      /* Here we perform cleanup of any leftover data from previous
       * partial failures.  This avoids having to call gs_shutil_rm_rf()
       * at random points throughout the process.
       *
       * TODO: Add /ostree/transaction file, and only do this cleanup if
       * we find it.
       */
      if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
        {
          g_prefix_error (error, "Performing initial cleanup: ");
          goto out;
        }

      if (!ostree_sysroot_deploy_tree (sysroot,
                                       opt_osname, new_revision, origin,
                                       merge_deployment,
                                       NULL,
                                       &new_deployment,
                                       cancellable, error))
        goto out;

      if (!ot_admin_complete_deploy_one (sysroot, opt_osname,
                                         new_deployment, merge_deployment, FALSE,
                                         cancellable, error))
        goto out;

      if (opt_reboot && g_file_equal (ostree_sysroot_get_path (sysroot), real_sysroot))
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
