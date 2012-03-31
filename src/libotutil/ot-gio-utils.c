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

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <string.h>

#include "otutil.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

GFileType
ot_gfile_type_for_mode (guint32 mode)
{
  if (S_ISDIR (mode))
    return G_FILE_TYPE_DIRECTORY;
  else if (S_ISREG (mode))
    return G_FILE_TYPE_REGULAR;
  else if (S_ISLNK (mode))
    return G_FILE_TYPE_SYMBOLIC_LINK;
  else if (S_ISBLK (mode) || S_ISCHR(mode) || S_ISFIFO(mode))
    return G_FILE_TYPE_SPECIAL;
  else
    return G_FILE_TYPE_UNKNOWN;
}

gboolean
ot_gfile_ensure_directory (GFile     *dir,
                           gboolean   with_parents, 
                           GError   **error)
{
  GError *temp_error = NULL;
  gboolean ret = FALSE;

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
  return ret;
}

GFile *
ot_gfile_get_child_strconcat (GFile *parent,
                              const char *first,
                              ...) 
{
  va_list args;
  GFile *ret;
  GString *buf;
  const char *arg;

  g_return_val_if_fail (first != NULL, NULL);

  va_start (args, first);
  
  buf = g_string_new (first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_string_append (buf, arg);

  ret = g_file_get_child (parent, buf->str);
  
  g_string_free (buf, TRUE);

  return ret;
}

/**
 * ot_gfile_unlink:
 * @path: Path to file
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Like g_file_delete(), except this function does not follow Unix
 * symbolic links, and will delete a symbolic link even if it's
 * pointing to a nonexistent file.  In other words, this function
 * merely wraps the raw Unix function unlink().
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
ot_gfile_unlink (GFile          *path,
                 GCancellable   *cancellable,
                 GError        **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (unlink (ot_gfile_get_path_cached (path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      return FALSE;
    }
  return TRUE;
}

/**
 * ot_gfile_rename:
 * @from: Current path
 * @to: New path
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * This function wraps the raw Unix function rename().
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
ot_gfile_rename (GFile          *from,
                 GFile          *to,
                 GCancellable   *cancellable,
                 GError        **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (rename (ot_gfile_get_path_cached (from),
              ot_gfile_get_path_cached (to)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      return FALSE;
    }
  return TRUE;

}

gboolean
ot_gfile_load_contents_utf8 (GFile         *file,
                             char         **contents_out,
                             char         **etag_out,
                             GCancellable  *cancellable,
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
    {
      *contents_out = ret_contents;
      ret_contents = NULL;
    }
  if (etag_out)
    {
      *etag_out = ret_etag;
      ret_etag = NULL;
    }
  ret = TRUE;
 out:
  g_free (ret_contents);
  g_free (ret_etag);
  return ret;
}

/* Like g_file_new_for_path, but only do local stuff, not GVFS */
GFile *
ot_gfile_new_for_path (const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get_local (), path);
}

const char *
ot_gfile_get_path_cached (GFile *file)
{
  const char *path;

  path = g_object_get_data ((GObject*)file, "ostree-file-path");
  if (!path)
    {
      path = g_file_get_path (file);
      g_object_set_data_full ((GObject*)file, "ostree-file-path", (char*)path, (GDestroyNotify)g_free);
    }
  return path;
}


const char *
ot_gfile_get_basename_cached (GFile *file)
{
  const char *name;

  name = g_object_get_data ((GObject*)file, "ostree-file-name");
  if (!name)
    {
      name = g_file_get_basename (file);
      g_object_set_data_full ((GObject*)file, "ostree-file-name", (char*)name, (GDestroyNotify)g_free);
    }
  return name;
}

gboolean
ot_gio_splice_and_checksum (GOutputStream  *out,
                            GInputStream   *in,
                            GChecksum     **out_checksum,
                            GCancellable   *cancellable,
                            GError        **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;

  g_return_val_if_fail (out != NULL || out_checksum != NULL, FALSE);

  if (out_checksum)
    ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  
  if (ret_checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof(buf), &bytes_read, cancellable, error))
            goto out;
          if (ret_checksum)
            g_checksum_update (ret_checksum, (guint8*)buf, bytes_read);
          if (out)
            {
              if (!g_output_stream_write_all (out, buf, bytes_read, &bytes_written, cancellable, error))
                goto out;
            }
        }
      while (bytes_read > 0);
    }
  else
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  ot_clear_checksum (&ret_checksum);
  return ret;
}

gboolean
ot_gio_checksum_stream (GInputStream   *in,
                        GChecksum     **out_checksum,
                        GCancellable   *cancellable,
                        GError        **error)
{
  if (!out_checksum)
    return TRUE;
  return ot_gio_splice_and_checksum (NULL, in, out_checksum, cancellable, error);
}

gboolean
ot_gfile_merge_dirs (GFile    *destination,
                     GFile    *src,
                     GCancellable *cancellable,
                     GError   **error)
{
  gboolean ret = FALSE;
  const char *dest_path = NULL;
  const char *src_path = NULL;
  GError *temp_error = NULL;
  GFileInfo *src_fileinfo = NULL;
  GFileInfo *dest_fileinfo = NULL;
  GFileEnumerator *src_enum = NULL;
  GFile *dest_subfile = NULL;
  GFile *src_subfile = NULL;
  const char *name;
  guint32 type;
  const int move_flags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA;

  dest_path = ot_gfile_get_path_cached (destination);
  src_path = ot_gfile_get_path_cached (src);

  dest_fileinfo = g_file_query_info (destination, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     cancellable, &temp_error);
  if (dest_fileinfo)
    {
      type = g_file_info_get_attribute_uint32 (dest_fileinfo, "standard::type");
      if (type != G_FILE_TYPE_DIRECTORY)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Attempting to replace non-directory %s with directory %s",
                       dest_path, src_path);
          goto out;
        }

      src_enum = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);
      if (!src_enum)
        goto out;

      while ((src_fileinfo = g_file_enumerator_next_file (src_enum, cancellable, &temp_error)) != NULL)
        {
          type = g_file_info_get_attribute_uint32 (src_fileinfo, "standard::type");
          name = g_file_info_get_attribute_byte_string (src_fileinfo, "standard::name");
      
          dest_subfile = g_file_get_child (destination, name);
          src_subfile = g_file_get_child (src, name);

          if (type == G_FILE_TYPE_DIRECTORY)
            {
              if (!ot_gfile_merge_dirs (dest_subfile, src_subfile, cancellable, error))
                goto out;
            }
          else
            {
              if (!g_file_move (src_subfile, dest_subfile,
                                move_flags, NULL, NULL, cancellable, error))
                goto out;
            }
          
          g_clear_object (&dest_subfile);
          g_clear_object (&src_subfile);
        }
      if (temp_error)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&temp_error);
      if (!g_file_move (src, destination, move_flags, NULL, NULL, cancellable, error))
        goto out;
    }
  else
    goto out;

  (void) rmdir (ot_gfile_get_path_cached (src));

  ret = TRUE;
 out:
  g_clear_object (&src_fileinfo);
  g_clear_object (&dest_fileinfo);
  g_clear_object (&src_enum);
  g_clear_object (&dest_subfile);
  g_clear_object (&src_subfile);
  return ret;
}
