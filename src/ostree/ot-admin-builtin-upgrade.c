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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static char *opt_osname;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after a successful upgrade", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;
  g_autofree char *origin_refspec = NULL;
  g_autofree char *new_revision = NULL;
  g_autoptr(GFile) deployment_path = NULL;
  g_autoptr(GFile) deployment_origin_path = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  GSConsole *console = NULL;
  gboolean in_status_line = FALSE;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  gboolean changed;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;

  context = g_option_context_new ("Construct new tree from current origin and deploy it, if it changed");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      in_status_line = TRUE;
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
    }

  if (opt_allow_downgrade)
    upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgraderpullflags,
                                     progress, &changed,
                                     cancellable, error))
    goto out;

  if (in_status_line)
    {
      gs_console_end_status_line (console, NULL, NULL);
      in_status_line = FALSE;
    }

  if (!changed)
    {
      g_print ("No update available.\n");
    }
  else
    {
      g_autoptr(GFile) real_sysroot = g_file_new_for_path ("/");

      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
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
  if (in_status_line)
    gs_console_end_status_line (console, NULL, NULL);
  if (context)
    g_option_context_free (context);
  return ret;
}
