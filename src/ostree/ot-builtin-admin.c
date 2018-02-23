/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
#include "ot-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "ostree-repo-file.h"

#include <glib/gi18n.h>

static OstreeCommand admin_subcommands[] = {
  { "cleanup", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_cleanup,
    "Delete untagged deployments and repository objects" },
  { "config-diff", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_diff,
    "Diff current /etc configuration versus default" },
  { "deploy", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_deploy,
    "Checkout revision REFSPEC as the new default deployment" },
  { "init-fs", OSTREE_BUILTIN_FLAG_NO_REPO,
     ot_admin_builtin_init_fs,
    "Initialize a root filesystem" },
  { "instutil", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_instutil,
    "Provide instutil commands, allow admin to change boot configuration and relabel selinux " },
  { "os-init", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_os_init,
    "Initialize empty state for given operating system" },
  { "set-origin", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_set_origin,
    "Set Origin and create a new origin file" },
  { "pin", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_pin,
    "Change the \"pinning\" state of a deployment" },
  { "status", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_status,
    "List deployments" },
  { "switch", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_switch,
    "Construct new tree from REF and deploy it" },
  { "undeploy", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_undeploy,
    "Delete deployment INDEX" },
  { "unlock", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_unlock,
    "Make the current deployment mutable (as a hotfix or development)" },
  { "upgrade", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_admin_builtin_upgrade,
    "Construct new tree from current origin and deploy it, if it changed" },
  { NULL, 0, NULL, NULL }
};

static GOptionContext *
ostree_admin_option_context_new_with_commands (void)
{
  OstreeCommand *command = admin_subcommands;
  GOptionContext *context = g_option_context_new ("--print-current-dir|COMMAND");

  g_autoptr(GString) summary = g_string_new ("Builtin \"admin\" Commands:");

  while (command->name != NULL)
    {
      g_string_append_printf (summary, "\n  %-19s", command->name);
        if (command->description != NULL)
          g_string_append_printf (summary, "%s", command->description);
      command++;
    }

  g_option_context_set_summary (context, summary->str);

  return context;
}

gboolean
ostree_builtin_admin (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *subcommand_name = NULL;
  OstreeCommand *subcommand;
  g_autofree char *prgname = NULL;
  int in, out;

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (subcommand_name == NULL)
            {
              subcommand_name = argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (argv[in], "--"))
        {
          break;
        }

      argv[out] = argv[in];
    }

  argc = out;

  subcommand = admin_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_autoptr(GOptionContext) context = NULL;
      g_autofree char *help = NULL;

      context = ostree_admin_option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (ostree_admin_option_context_parse (context, NULL, &argc, &argv,
                                             OSTREE_ADMIN_BUILTIN_FLAG_NO_SYSROOT,
                                             invocation, NULL, cancellable, error))
        {
          if (subcommand_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No \"admin\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Unknown \"admin\" subcommand '%s'", subcommand_name);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  OstreeCommandInvocation sub_invocation = { .command = subcommand };
  if (!subcommand->fn (argc, argv, &sub_invocation, cancellable, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
