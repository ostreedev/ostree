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
static gboolean opt_pull_only;
static gboolean opt_deploy_only;
static char *opt_osname;
static char *opt_override_commit;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after a successful upgrade", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "override-commit", 0, 0, G_OPTION_ARG_STRING, &opt_override_commit, "Deploy CHECKSUM instead of the latest tree", "CHECKSUM" },
  { "pull-only", 0, 0, G_OPTION_ARG_NONE, &opt_pull_only, "Do not create a deployment, just download", NULL },
  { "deploy-only", 0, 0, G_OPTION_ARG_NONE, &opt_deploy_only, "Do not pull, only deploy", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  g_autoptr(GKeyFile) origin = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  gboolean changed;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;

  context = g_option_context_new ("Construct new tree from current origin and deploy it, if it changed");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;

  if (opt_pull_only && opt_deploy_only)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot simultaneously specify --pull-only and --deploy-only");
      goto out;
    }
  else if (opt_pull_only && opt_reboot)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot simultaneously specify --pull-only and --reboot");
      goto out;
    }

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  origin = ostree_sysroot_upgrader_dup_origin (upgrader);
  if (origin != NULL)
    {
      gboolean origin_changed = FALSE;

      if (opt_override_commit != NULL)
        {
          /* Override the commit to pull and deploy. */
          g_key_file_set_string (origin, "origin",
                                 "override-commit",
                                 opt_override_commit);
          origin_changed = TRUE;
        }
      else
        {
          /* Strip any override-commit from the origin file so
           * we always upgrade to the latest available commit. */
          origin_changed = g_key_file_remove_key (origin, "origin",
                                                  "override-commit", NULL);
        }

      /* Should we consider requiring --discard-hotfix here? */
      origin_changed |= g_key_file_remove_key (origin, "origin", "unlocked", NULL);

      if (origin_changed)
        {
          /* XXX GCancellable parameter is not used. */
          if (!ostree_sysroot_upgrader_set_origin (upgrader, origin, NULL, error))
            goto out;
        }
    }

  if (opt_deploy_only)
    upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_SYNTHETIC;

  { g_auto(GLnxConsoleRef) console = { 0, };
    glnx_console_lock (&console);

    if (console.is_tty)
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

    if (opt_allow_downgrade)
      upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

    if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgraderpullflags,
                                       progress, &changed,
                                       cancellable, error))
      {
        /* In the pull-only case, we do a cleanup here to ensure that if
         * multiple commits were pulled, we garbage collect any old
         * partially-pulled intermediate commits before pulling more. This is
         * really a best practice in general, but for maximum compatiblity, we
         * only do cleanup if a user specifies the new --pull-only option.
         * Otherwise, we would break the case of trying to deploy a commit that
         * isn't directly referenced.
         */
        if (opt_pull_only)
          (void) ostree_sysroot_cleanup (sysroot, NULL, NULL);
        goto out;
      }

    if (progress)
      ostree_async_progress_finish (progress);
  }

  if (!changed)
    {
      g_print ("No update available.\n");
    }
  else
    {
      if (!opt_pull_only)
        {
          if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
            goto out;
        }

      if (opt_reboot)
        {
          if (!ot_admin_execve_reboot (sysroot, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
