/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
#include "ot-remote-builtins.h"

static OstreeCommand remote_subcommands[] = {
  { "add", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_remote_builtin_add,
    "Add a remote repository" },
  { "delete", OSTREE_BUILTIN_FLAG_NO_REPO,
    ot_remote_builtin_delete,
    "Delete a remote repository" },
  { "show-url", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_show_url,
    "Show remote repository URL" },
  { "list", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_list,
    "List remote repository names" },
  { "gpg-import", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_gpg_import,
    "Import GPG keys" },
#ifdef HAVE_LIBSOUP
  { "add-cookie", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_add_cookie,
    "Add a cookie to remote" },
  { "delete-cookie", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_delete_cookie,
    "Remove one cookie from remote" },
  { "list-cookies", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_list_cookies,
    "Show remote repository cookies" },
#endif
  { "refs", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_refs,
    "List remote refs" },
  { "summary", OSTREE_BUILTIN_FLAG_NONE,
    ot_remote_builtin_summary,
    "Show remote summary" },
  { NULL, 0, NULL, NULL }
};

static GOptionContext *
remote_option_context_new_with_commands (void)
{
  OstreeCommand *subcommand = remote_subcommands;
  GOptionContext *context = g_option_context_new ("COMMAND");

  g_autoptr(GString) summary = g_string_new ("Builtin \"remote\" Commands:");

  while (subcommand->name != NULL)
    {
      g_string_append_printf (summary, "\n  %-18s", subcommand->name);
      if (subcommand->description != NULL)
        g_string_append_printf (summary, "%s", subcommand->description);

      subcommand++;
    }

  g_option_context_set_summary (context, summary->str);

  return context;
}

gboolean
ostree_builtin_remote (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  OstreeCommand *subcommand;
  const char *subcommand_name = NULL;
  g_autofree char *prgname = NULL;
  gboolean ret = FALSE;
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

  subcommand = remote_subcommands;
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

      context = remote_option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (ostree_option_context_parse (context, NULL, &argc, &argv,
                                       invocation, NULL, cancellable,
                                       error))
        {
          if (subcommand_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No \"remote\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown \"remote\" subcommand '%s'", subcommand_name);
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
