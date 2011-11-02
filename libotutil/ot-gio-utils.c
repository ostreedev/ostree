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

#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include <string.h>

#include "otutil.h"

gboolean
ot_util_ensure_directory (const char *path, gboolean with_parents, GError **error)
{
  GFile *dir;
  GError *temp_error = NULL;
  gboolean ret = FALSE;

  dir = g_file_new_for_path (path);
  if (with_parents)
    ret = g_file_make_directory_with_parents (dir, NULL, &temp_error);
  else
    ret = g_file_make_directory (dir, NULL, &temp_error);
  if (!ret)
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      else
        g_clear_error (&temp_error);
    }

  ret = TRUE;
 out:
  g_clear_object (&dir);
  return ret;
}


char *
ot_util_get_file_contents_utf8 (const char *path,
                                GError    **error)
{
  GFile *file = NULL;
  char *ret = NULL;

  file = ot_util_new_file_for_path (path);
  if (!ot_util_gfile_load_contents_utf8 (file, NULL, &ret, NULL, error))
    goto out;

 out:
  g_clear_object (&file);
  return ret;
}

gboolean
ot_util_gfile_load_contents_utf8 (GFile         *file,
                                  GCancellable  *cancellable,
                                  char         **contents_out,
                                  char         **etag_out,
                                  GError       **error)
{
  char *ret_contents = NULL;
  char *ret_etag = NULL;
  gsize len;
  gboolean ret = FALSE;

  if (!g_file_load_contents (file, cancellable, &ret_contents, &len, &ret_etag, error))
    goto out;
  if (!g_utf8_validate (ret_contents, len, NULL))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Invalid UTF-8");
      goto out;
    }

  if (contents_out)
    *contents_out = ret_contents;
  else
    g_free (ret_contents);
  ret_contents = NULL;
  if (etag_out)
    *etag_out = ret_etag;
  else
    g_free (ret_etag);
  ret_etag = NULL;
  ret = TRUE;
 out:
  g_free (ret_contents);
  g_free (ret_etag);
  return ret;
}

GInputStream *
ot_util_read_file_noatime (GFile *file, GCancellable *cancellable, GError **error)
{
  GInputStream *ret = NULL;
  int fd;
  int flags = O_RDONLY;
  char *path = NULL;

  path = g_file_get_path (file);
#ifdef O_NOATIME
  flags |= O_NOATIME;
#endif
  fd = open (path, flags);
  if (fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = (GInputStream*)g_unix_input_stream_new (fd, TRUE);
  
 out:
  g_free (path);
  return ret;
}

/* Like g_file_new_for_path, but only do local stuff, not GVFS */
GFile *
ot_util_new_file_for_path (const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get_local (), path);
}
