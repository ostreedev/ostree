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

#include "otutil.h"

#include <string.h>

guchar *
ot_csum_from_gchecksum (GChecksum  *checksum)
{
  guchar *ret = g_malloc (32);
  gsize len = 32;
  
  g_checksum_get_digest (checksum, ret, &len);
  g_assert (len == 32);
  return ret;
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
  else if (out != NULL)
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
  g_autofree guchar *ret_csum = NULL;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (!ot_gio_splice_update_checksum (out, in, checksum, cancellable, error))
    goto out;

  ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value (out_csum, &ret_csum);
 out:
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
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

char *
ot_checksum_file_at (int             dfd,
                     const char     *path,
                     GChecksumType   checksum_type,
                     GCancellable   *cancellable,
                     GError        **error)
{
  GChecksum *checksum = NULL;
  char *ret = NULL;
  g_autoptr(GInputStream) in = NULL;

  if (!ot_openat_read_stream (dfd, path, TRUE, &in, cancellable, error))
    goto out;

  checksum = g_checksum_new (checksum_type);

  if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
    goto out;

  ret = g_strdup (g_checksum_get_string (checksum));
 out:
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
  return ret;

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
