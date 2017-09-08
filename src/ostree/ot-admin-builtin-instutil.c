/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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
#include "ot-builtins.h"
#include "ot-admin-instutil-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} OstreeAdminInstUtilCommand;

static OstreeAdminInstUtilCommand admin_instutil_subcommands[] = {
#ifdef HAVE_SELINUX
  { "selinux-ensure-labeled", ot_admin_instutil_builtin_selinux_ensure_labeled },
#endif
  { "set-kargs", ot_admin_instutil_builtin_set_kargs },
  { "grub2-generate", ot_admin_instutil_builtin_grub2_generate },
  { NULL, NULL }
};

static GOptionContext *
ostree_admin_instutil_option_context_new_with_commands (void)
{
  OstreeAdminInstUtilCommand *command = admin_instutil_subcommands;
  GOptionContext *context = g_option_context_new ("COMMAND");

  g_autoptr(GString) summary = g_string_new ("Builtin \"admin instutil\" Commands:");

  while (command->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", command->name);
      command++;
    }

  g_option_context_set_summary (context, summary->str);

  return context;
}

gboolean
ot_admin_builtin_instutil (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  const char *subcommand_name = NULL;
  int in, out;

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

  OstreeAdminInstUtilCommand *subcommand = admin_instutil_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_autoptr(GOptionContext) context =
        ostree_admin_instutil_option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (ostree_admin_option_context_parse (context, NULL, &argc, &argv,
                                             OSTREE_ADMIN_BUILTIN_FLAG_NO_SYSROOT,
                                             NULL, cancellable, error))
        {
          if (subcommand_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No \"admin instutil\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Unknown \"admin instutil\" subcommand '%s'", subcommand_name);
            }
        }

      g_autofree char *help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      return FALSE;
    }

  g_autofree char *prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  if (!subcommand->fn (argc, argv, cancellable, error))
    return FALSE;

  return TRUE;
}
