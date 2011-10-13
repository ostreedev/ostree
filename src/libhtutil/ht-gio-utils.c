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

#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include <string.h>

#include "htutil.h"

gboolean
ht_util_ensure_directory (const char *path, gboolean with_parents, GError **error)
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
ht_util_get_file_contents_utf8 (const char *path,
                                GError    **error)
{
  char *contents;
  gsize len;
  if (!g_file_get_contents (path, &contents, &len, error))
    return NULL;
  if (!g_utf8_validate (contents, len, NULL))
    {
      g_free (contents);
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "File %s contains invalid UTF-8",
                   path);
      return NULL;
    }
  return contents;
}

GInputStream *
ht_util_read_file_noatime (GFile *file, GCancellable *cancellable, GError **error)
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
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = (GInputStream*)g_unix_input_stream_new (fd, TRUE);
  
 out:
  g_free (path);
  return ret;
}
