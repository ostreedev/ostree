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
static gboolean quiet;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", NULL },
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { NULL }
};

typedef struct {
  guint n_objects;
} HtFsckData;

static void
object_iter_callback (HacktreeRepo  *repo,
                      const char    *path,
                      GFileInfo     *file_info,
                      gpointer       user_data)
{
  HtFsckData *data = user_data;
  guint32 nlinks;
  struct stat stbuf;
  GChecksum *checksum = NULL;
  GError *error = NULL;
  char *dirname;
  char *checksum_prefix;
  char *checksum_string;

  dirname = g_path_get_dirname (path);
  checksum_prefix = g_path_get_basename (dirname);
  
  nlinks = g_file_info_get_attribute_uint32 (file_info, "unix::nlink");

  if (nlinks < 2 && !quiet)
    g_printerr ("note: floating object: %s\n", path);

  if (!hacktree_stat_and_checksum_file (-1, path, &checksum, &stbuf, &error))
    goto out;

  checksum_string = g_strconcat (checksum_prefix, g_file_info_get_name (file_info), NULL);

  if (strcmp (checksum_string, g_checksum_get_string (checksum)) != 0)
    {
      g_printerr ("ERROR: corrupted object '%s' expected checksum: %s\n",
                  path, g_checksum_get_string (checksum));
    }

  data->n_objects++;

 out:
  if (checksum != NULL)
    g_checksum_free (checksum);
  g_free (dirname);
  g_free (checksum_prefix);
  if (error != NULL)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
    }
}

gboolean
hacktree_builtin_fsck (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  HtFsckData data;
  gboolean ret = FALSE;
  HacktreeRepo *repo = NULL;

  context = g_option_context_new ("- Check the repository for consistency");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  data.n_objects = 0;

  repo = hacktree_repo_new (repo_path);
  if (!hacktree_repo_check (repo, error))
    goto out;

  if (!hacktree_repo_iter_objects (repo, object_iter_callback, &data, error))
    goto out;

  if (!quiet)
    g_printerr ("Total Objects: %u\n", data.n_objects);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  return ret;
}
