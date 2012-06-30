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
  gboolean ret = FALSE;
  GError *temp_error = NULL;

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
ot_gfile_from_build_path (const char *first, ...)
{
  va_list args;
  const char *arg;
  ot_lfree char *path = NULL;
  ot_lptrarray GPtrArray *components = NULL;  

  va_start (args, first);

  components = g_ptr_array_new ();
  
  arg = first;
  while (arg != NULL)
    {
      g_ptr_array_add (components, (char*)arg);
      arg = va_arg (args, const char *);
    }

  va_end (args);

  g_ptr_array_add (components, NULL);

  path = g_build_filenamev ((char**)components->pdata);

  return g_file_new_for_path (path);
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
                             char         **out_contents,
                             char         **out_etag,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  gsize len;
  ot_lfree char *ret_contents = NULL;
  ot_lfree char *ret_etag = NULL;

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

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
  ot_transfer_out_value (out_etag, &ret_etag);
 out:
  return ret;
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
ot_gio_write_update_checksum (GOutputStream  *out,
                              gconstpointer   data,
                              gsize           len,
                              gsize          *out_bytes_written,
                              GChecksum      *checksum,
                              GCancellable   *cancellable,
                              GError        **error)
{
  gboolean ret = FALSE;

  if (out)
    {
      if (!g_output_stream_write_all (out, data, len, out_bytes_written,
                                      cancellable, error))
        goto out;
    }
  else if (out_bytes_written)
    {
      *out_bytes_written = len;
    }

  if (checksum)
    g_checksum_update (checksum, data, len);
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_gio_splice_update_checksum (GOutputStream  *out,
                               GInputStream   *in,
                               GChecksum      *checksum,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (out != NULL || checksum != NULL, FALSE);

  if (checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof(buf), &bytes_read, cancellable, error))
            goto out;
          if (!ot_gio_write_update_checksum (out, buf, bytes_read, &bytes_written, checksum,
                                             cancellable, error))
            goto out;
        }
      while (bytes_read > 0);
    }
  else
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_gio_splice_get_checksum (GOutputStream  *out,
                            GInputStream   *in,
                            guchar        **out_csum,
                            GCancellable   *cancellable,
                            GError        **error)
{
  gboolean ret = FALSE;
  GChecksum *checksum = NULL;
  ot_lfree guchar *ret_csum = NULL;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (!ot_gio_splice_update_checksum (out, in, checksum, cancellable, error))
    goto out;

  ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value (out_csum, &ret_csum);
 out:
  ot_clear_checksum (&checksum);
  return ret;
}

gboolean
ot_gio_checksum_stream (GInputStream   *in,
                        guchar        **out_csum,
                        GCancellable   *cancellable,
                        GError        **error)
{
  if (!out_csum)
    return TRUE;
  return ot_gio_splice_get_checksum (NULL, in, out_csum, cancellable, error);
}

static void
checksum_stream_thread (GSimpleAsyncResult   *result,
                        GObject              *object,
                        GCancellable         *cancellable)
{
  GError *error = NULL;
  guchar *csum;

  if (!ot_gio_checksum_stream ((GInputStream*)object, &csum,
                               cancellable, &error))
    g_simple_async_result_take_error (result, error);
  else
    g_simple_async_result_set_op_res_gpointer (result, csum, g_free);
}

void
ot_gio_checksum_stream_async (GInputStream         *in,
                              int                   io_priority,
                              GCancellable         *cancellable,
                              GAsyncReadyCallback   callback,
                              gpointer              user_data)
{
  GSimpleAsyncResult *result;

  result = g_simple_async_result_new ((GObject*) in,
                                      callback, user_data,
                                      ot_gio_checksum_stream_async);

  g_simple_async_result_run_in_thread (result, checksum_stream_thread, io_priority, cancellable);
  g_object_unref (result);
}

guchar *
ot_gio_checksum_stream_finish (GInputStream   *in,
                               GAsyncResult   *result,
                               GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ot_gio_checksum_stream_async);
  return g_memdup (g_simple_async_result_get_op_res_gpointer (simple), 32);

}
