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

#include "ht-builtins.h"
#include "hacktree.h"

#include <glib/gi18n.h>

static char *repo_path;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", "repo" },
  { NULL }
};

gboolean
hacktree_builtin_checkout (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  HacktreeRepo *repo = NULL;
  int i;
  const char *commit;
  const char *destination;

  context = g_option_context_new ("COMMIT DESTINATION - Check out a commit into a filesystem tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  repo = hacktree_repo_new (repo_path);
  if (!hacktree_repo_check (repo, error))
    goto out;

  if (argc < 3)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "COMMIT and DESTINATION must be specified");
      goto out;
    }

  commit = argv[1];
  destination = argv[2];

  if (!hacktree_repo_checkout (repo, commit, destination, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  return ret;
}
