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
#include "ostree.h"

#include <glib/gi18n.h>

static gboolean user_mode;

static GOptionEntry options[] = {
  { "user-mode", 'U', 0, G_OPTION_ARG_NONE, &user_mode, "Do not change file ownership or initialze extended attributes", NULL },
  { NULL }
};

gboolean
ostree_builtin_checkout (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  GCancellable *cancellable = NULL;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *commit;
  char *resolved_commit = NULL;
  const char *destination;
  OstreeRepoFile *root = NULL;
  GFileInfo *root_info = NULL;
  GFile *destf = NULL;

  context = g_option_context_new ("COMMIT DESTINATION - Check out a commit into a filesystem tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
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

  destf = ot_gfile_new_for_path (destination);

  if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
    goto out;

  root = (OstreeRepoFile*)ostree_repo_file_new_root (repo, resolved_commit);
  if (!ostree_repo_file_ensure_resolved (root, error))
    goto out;

  root_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!root_info)
    goto out;

  if (!ostree_repo_checkout_tree (repo, user_mode ? OSTREE_REPO_CHECKOUT_MODE_USER : 0,
                                  destf, root, root_info, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_free (resolved_commit);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  g_clear_object (&destf);
  g_clear_object (&root);
  g_clear_object (&root_info);
  return ret;
}
