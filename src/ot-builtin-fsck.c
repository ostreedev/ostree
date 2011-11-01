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
static gboolean quiet;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", NULL },
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { NULL }
};

typedef struct {
  guint n_objects;
} HtFsckData;

static gboolean
checksum_packed_file (HtFsckData   *data,
                      const char   *path,
                      GChecksum   **out_checksum,
                      GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GFile *file = NULL;
  char *metadata_buf = NULL;
  GVariant *metadata = NULL;
  GVariant *xattrs = NULL;
  GFileInputStream *in = NULL;
  guint32 metadata_len;
  guint32 version, uid, gid, mode;
  guint64 content_len;
  gsize bytes_read;
  char buf[8192];

  file = ot_util_new_file_for_path (path);

  in = g_file_read (file, NULL, error);
  if (!in)
    goto out;
      
  if (!g_input_stream_read_all ((GInputStream*)in, &metadata_len, 4, &bytes_read, NULL, error))
    goto out;
      
  metadata_len = GUINT32_FROM_BE (metadata_len);
      
  metadata_buf = g_malloc (metadata_len);
      
  if (!g_input_stream_read_all ((GInputStream*)in, metadata_buf, metadata_len, &bytes_read, NULL, error))
    goto out;

  metadata = g_variant_new_from_data (G_VARIANT_TYPE (OSTREE_PACK_FILE_VARIANT_FORMAT),
                                      metadata_buf, metadata_len, FALSE, NULL, NULL);
      
  g_variant_get (metadata, "(uuuu@a(ayay)t)",
                 &version, &uid, &gid, &mode,
                 &xattrs, &content_len);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  content_len = GUINT64_FROM_BE (content_len);

  ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  do
    {
      if (!g_input_stream_read_all ((GInputStream*)in, buf, sizeof(buf), &bytes_read, NULL, error))
        goto out;
      g_checksum_update (ret_checksum, (guint8*)buf, bytes_read);
    }
  while (bytes_read > 0);

  ostree_checksum_update_stat (ret_checksum, uid, gid, mode);
  g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  g_free (metadata_buf);
  g_clear_object (&file);
  g_clear_object (&in);
  if (metadata)
   g_variant_unref (metadata);
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
}
                    

static void
object_iter_callback (OstreeRepo  *repo,
                      const char    *path,
                      GFileInfo     *file_info,
                      gpointer       user_data)
{
  HtFsckData *data = user_data;
  struct stat stbuf;
  GChecksum *checksum = NULL;
  GError *error = NULL;
  char *dirname = NULL;
  char *checksum_prefix = NULL;
  char *checksum_string = NULL;
  char *filename_checksum = NULL;
  gboolean packed = FALSE;
  OstreeObjectType objtype;
  char *dot;

  /* nlinks = g_file_info_get_attribute_uint32 (file_info, "unix::nlink");
     if (nlinks < 2 && !quiet)
     g_printerr ("note: floating object: %s\n", path); */

  if (g_str_has_suffix (path, ".meta"))
    objtype = OSTREE_OBJECT_TYPE_META;
  else if (g_str_has_suffix (path, ".file"))
    objtype = OSTREE_OBJECT_TYPE_FILE;
  else if (g_str_has_suffix (path, ".packfile"))
    {
      objtype = OSTREE_OBJECT_TYPE_FILE;
     packed = TRUE;
    }
  else
    g_assert_not_reached ();

  if (packed && objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!checksum_packed_file (data, path, &checksum, &error))
        goto out;
    }
  else
    {
      if (!ostree_stat_and_checksum_file (-1, path, objtype, &checksum, &stbuf, &error))
        goto out;
    }

  filename_checksum = g_strdup (g_file_info_get_name (file_info));
  dot = strrchr (filename_checksum, '.');
  g_assert (dot != NULL);
  *dot = '\0';
  
  dirname = g_path_get_dirname (path);
  checksum_prefix = g_path_get_basename (dirname);
  checksum_string = g_strconcat (checksum_prefix, filename_checksum, NULL);
  
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
  g_free (checksum_string);
  g_free (filename_checksum);
  if (error != NULL)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
    }
}

gboolean
ostree_builtin_fsck (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  HtFsckData data;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;

  context = g_option_context_new ("- Check the repository for consistency");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  data.n_objects = 0;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!ostree_repo_iter_objects (repo, object_iter_callback, &data, error))
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
