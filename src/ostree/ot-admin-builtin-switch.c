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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static gboolean opt_reboot;
static char *opt_osname;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-switch.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Reboot after switching trees", NULL },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { NULL }
};

gboolean
ot_admin_builtin_switch (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context =
    g_option_context_new ("REF - Construct new tree from REF and deploy it");
  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REF must be specified", error);
      return FALSE;
    }

  const char *new_provided_refspec = argv[1];

  g_autoptr(OstreeSysrootUpgrader) upgrader =
    ostree_sysroot_upgrader_new_for_os_with_flags (sysroot, opt_osname,
                                                   OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED,
                                                   cancellable, error);
  if (!upgrader)
    return FALSE;

  GKeyFile *old_origin = ostree_sysroot_upgrader_get_origin (upgrader);
  g_autofree char *origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;
  if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
    return FALSE;

  g_autofree char *new_remote = NULL;
  g_autofree char *new_ref = NULL;
  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
    {
      new_remote = g_strdup (new_provided_refspec);
      new_remote[strlen(new_remote)-1] = '\0';
      new_ref = g_strdup (origin_ref);
    }
  else
    {
      if (!ostree_parse_refspec (new_provided_refspec, &new_remote, &new_ref, error))
        return FALSE;
    }

  const char* remote = new_remote ?: origin_remote;
  g_autofree char *new_refspec = NULL;
  if (remote)
    new_refspec = g_strconcat (remote, ":", new_ref, NULL);
  else
    new_refspec = g_strdup (new_ref);

  if (strcmp (origin_refspec, new_refspec) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Old and new refs are equal: %s", new_refspec);
      return FALSE;
    }

  g_autoptr(GKeyFile) new_origin = ostree_sysroot_origin_new_from_refspec (sysroot, new_refspec);
  if (!ostree_sysroot_upgrader_set_origin (upgrader, new_origin, cancellable, error))
    return FALSE;

  { g_auto(GLnxConsoleRef) console = { 0, };
    glnx_console_lock (&console);

    g_autoptr(OstreeAsyncProgress) progress = NULL;
    if (console.is_tty)
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

    /* Always allow older...there's not going to be a chronological
     * relationship necessarily.
     */
    gboolean changed;
    if (!ostree_sysroot_upgrader_pull (upgrader, 0,
                                       OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER,
                                       progress, &changed,
                                       cancellable, error))
      return FALSE;

    if (progress)
      ostree_async_progress_finish (progress);
  }

  if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    return FALSE;

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  g_print ("Deleting ref '%s:%s'\n", origin_remote, origin_ref);
  ostree_repo_transaction_set_ref (repo, origin_remote, origin_ref, NULL);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  if (opt_reboot)
    {
      if (!ot_admin_execve_reboot (sysroot, error))
        return FALSE;
    }

  return TRUE;
}
