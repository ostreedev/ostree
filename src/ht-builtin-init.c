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
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", NULL },
  { NULL }
};

gboolean
hacktree_builtin_init (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context = NULL;
  gboolean ret = FALSE;
  char *htdir_path = NULL;
  char *objects_path = NULL;
  GFile *htdir = NULL;
  GFile *objects_dir = NULL;

  context = g_option_context_new ("- Check the repository for consistency");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  htdir_path = g_build_filename (repo_path, HACKTREE_REPO_DIR, NULL);
  htdir = ht_util_new_file_for_path (htdir_path);

  if (!g_file_make_directory (htdir, NULL, error))
    goto out;

  objects_path = g_build_filename (htdir_path, "objects", NULL);
  objects_dir = g_file_new_for_path (objects_path);
  if (!g_file_make_directory (objects_dir, NULL, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_free (htdir_path);
  g_clear_object (&htdir);
  return ret;
}
