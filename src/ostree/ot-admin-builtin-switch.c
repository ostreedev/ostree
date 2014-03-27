/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2014 Colin Walters <walters@verbum.org>
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
static char *opt_osname;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Specify operating system root to use", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_switch (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *new_ref = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *origin_refspec = NULL;
  gs_free char *origin_remote = NULL;
  gs_free char *origin_ref = NULL;
  gs_free char *new_refspec = NULL;
  gs_free char *new_revision = NULL;
  gs_unref_object GFile *deployment_path = NULL;
  gs_unref_object GFile *deployment_origin_path = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  gs_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  gboolean changed;
  GSConsole *console;
  GKeyFile *old_origin;
  GKeyFile *new_origin = NULL;

  context = g_option_context_new ("REF - Construct new tree from current origin and deploy it, if it changed");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REF must be specified", error);
      goto out;
    }

  new_ref = argv[1];

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  old_origin = ostree_sysroot_upgrader_get_origin (upgrader);
  origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
  
  if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
    goto out;
  
  if (origin_remote)
    new_refspec = g_strconcat (origin_remote, ":", new_ref, NULL);
  else
    new_refspec = g_strdup (new_ref);

  if (strcmp (origin_refspec, new_refspec) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Old and new refs are equal: %s", new_refspec);
      goto out;
    }

  new_origin = ostree_sysroot_origin_new_from_refspec (sysroot, new_refspec);
  if (!ostree_sysroot_upgrader_set_origin (upgrader, new_origin, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ot_common_pull_progress, console);
    }

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, 0, progress, &changed,
                                     cancellable, error))
    goto out;

  if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  g_print ("Deleting ref '%s:%s'\n", origin_remote, origin_ref);
  ostree_repo_transaction_set_ref (repo, origin_remote, origin_ref, NULL);
  
  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;
  
  {
    gs_unref_object GFile *real_sysroot = g_file_new_for_path ("/");
      
    if (opt_reboot && g_file_equal (ostree_sysroot_get_path (sysroot), real_sysroot))
      {
        gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                       cancellable, error,
                                       "systemctl", "reboot", NULL);
      }
  }

  ret = TRUE;
 out:
  if (console)
    gs_console_end_status_line (console, NULL, NULL);
  if (new_origin)
    g_key_file_unref (new_origin);
  if (context)
    g_option_context_free (context);
  return ret;
}
