/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

#include "ot-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-main.h"
#include "ostree.h"
#include "ostree-repo-file.h"

#include <glib/gi18n.h>

static char *opt_ostree_dir = "/ostree";

static GOptionEntry options[] = {
  { "ostree-dir", 0, 0, G_OPTION_ARG_STRING, &opt_ostree_dir, "Path to OSTree root directory (default: /ostree)", NULL },
  { NULL }
};

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GFile *ostree_dir, GError **error);
} OstreeAdminCommand;

static OstreeAdminCommand admin_subcommands[] = {
  { "init", ot_admin_builtin_init },
  { "deploy", ot_admin_builtin_deploy },
  { "update-kernel", ot_admin_builtin_update_kernel },
  { "config-diff", ot_admin_builtin_diff },
  { NULL, NULL }
};

gboolean
ostree_builtin_admin (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  __attribute__((unused)) GCancellable *cancellable = NULL;
  const char *subcommand_name;
  OstreeAdminCommand *subcommand;
  int subcmd_argc;
  char **subcmd_argv = NULL;
  ot_lobj GFile *ostree_dir = NULL;

  context = g_option_context_new ("[OPTIONS] SUBCOMMAND - Run an administrative subcommand");

  {
    GString *s = g_string_new ("Subcommands:\n");

    subcommand = admin_subcommands;
    while (subcommand->name)
      {
        g_string_append_printf (s, "  %s\n", subcommand->name);
        subcommand++;
      }
    g_option_context_set_description (context, s->str);
    g_string_free (s, TRUE);
  }
    
  g_option_context_add_main_entries (context, options, NULL);
  /* Skip subcommand options */
  g_option_context_set_ignore_unknown_options (context, TRUE);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A valid SUBCOMMAND is required", error);
      goto out;
    }
  subcommand_name = argv[1];

  subcommand = admin_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown command '%s'", subcommand_name);
      goto out;
    }

  ostree_dir = g_file_new_for_path (opt_ostree_dir);

  ostree_prep_builtin_argv (subcommand_name, argc-2, argv+2, &subcmd_argc, &subcmd_argv);

  if (!subcommand->fn (subcmd_argc, subcmd_argv, ostree_dir, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
