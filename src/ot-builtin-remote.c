/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static char *repo_path;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", "repo" },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);
  g_free (help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       message);
}

gboolean
ostree_builtin_remote (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  OstreeCheckout *checkout = NULL;
  const char *op;
  GKeyFile *config = NULL;

  context = g_option_context_new ("OPERATION [args] - Control remote repository configuration");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "OPERATION must be specified", error);
      goto out;
    }

  op = argv[1];

  config = ostree_repo_copy_config (repo);

  if (!strcmp (op, "add"))
    {
      char *key;
      if (argc < 4)
        {
          usage_error (context, "NAME and URL must be specified", error);
          goto out;
        }
      key = g_strdup_printf ("remote \"%s\"", argv[2]);
      g_key_file_set_string (config, key, "url", argv[3]);
      g_free (key);
    }
  else
    {
      usage_error (context, "Unknown operation", error);
      goto out;
    }

  if (!ostree_repo_write_config (repo, config, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (config)
    g_key_file_unref (config);
  g_clear_object (&repo);
  g_clear_object (&checkout);
  return ret;
}
