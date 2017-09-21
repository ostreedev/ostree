/*
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


void
ot_bin2hex (char *out_buf, const guint8 *inbuf, gsize len)
{
  static const gchar hexchars[] = "0123456789abcdef";
  guint i, j;

  for (i = 0, j = 0; i < len; i++, j += 2)
    {
      guchar byte = inbuf[i];
      out_buf[j] = hexchars[byte >> 4];
      out_buf[j+1] = hexchars[byte & 0xF];
    }
  out_buf[j] = '\0';
}

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
  if (out)
    {
      if (!g_output_stream_write_all (out, data, len, out_bytes_written,
                                      cancellable, error))
        return FALSE;
    }
  else if (out_bytes_written)
    {
      *out_bytes_written = len;
    }

  if (checksum)
    g_checksum_update (checksum, data, len);
  return TRUE;
}

gboolean
ot_gio_splice_update_checksum (GOutputStream  *out,
                               GInputStream   *in,
                               GChecksum      *checksum,
                               GCancellable   *cancellable,
                               GError        **error)
{
  g_return_val_if_fail (out != NULL || checksum != NULL, FALSE);

  if (checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof(buf), &bytes_read, cancellable, error))
            return FALSE;
          if (!ot_gio_write_update_checksum (out, buf, bytes_read, &bytes_written, checksum,
                                             cancellable, error))
            return FALSE;
        }
      while (bytes_read > 0);
    }
  else if (out != NULL)
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        return FALSE;
    }

  return TRUE;
}

/* Copy @in to @out, return in @out_csum the binary checksum for
 * all data read.
 */
gboolean
ot_gio_splice_get_checksum (GOutputStream  *out,
                            GInputStream   *in,
                            guchar        **out_csum,
                            GCancellable   *cancellable,
                            GError        **error)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (!ot_gio_splice_update_checksum (out, in, checksum, cancellable, error))
    return FALSE;

  g_autofree guchar *ret_csum = ot_csum_from_gchecksum (checksum);
  ot_transfer_out_value (out_csum, &ret_csum);
  return TRUE;
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
  g_autoptr(GInputStream) in = NULL;
  if (!ot_openat_read_stream (dfd, path, TRUE, &in, cancellable, error))
    return FALSE;

  g_autoptr(GChecksum) checksum = g_checksum_new (checksum_type);
  if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
    return FALSE;

  return g_strdup (g_checksum_get_string (checksum));
}
