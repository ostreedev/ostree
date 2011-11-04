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

#include <string.h>

#include "otutil.h"

GHashTable *
ot_util_variant_asv_to_hash_table (GVariant *variant)
{
  GHashTable *ret;
  GVariantIter *viter;
  char *key;
  GVariant *value;
  
  ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  viter = g_variant_iter_new (variant);
  while (g_variant_iter_next (viter, "{s@v}", &key, &value))
    g_hash_table_replace (ret, key, g_variant_ref_sink (value));
  
  g_variant_iter_free (viter);
  
  return ret;
}

gboolean
ot_util_variant_save (GFile *dest,
                      GVariant *variant,
                      GCancellable *cancellable,
                      GError  **error)
{
  gboolean ret = FALSE;
  GOutputStream *out = NULL;
  gsize bytes_written;
  
  out = (GOutputStream*)g_file_replace (dest, NULL, 0, FALSE, cancellable, error);
  if (!out)
    goto out;

  if (!g_output_stream_write_all (out,
                                  g_variant_get_data (variant),
                                  g_variant_get_size (variant),
                                  &bytes_written,
                                  cancellable,
                                  error))
    goto out;
  if (!g_output_stream_close (out, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&out);
  return ret;
}

gboolean
ot_util_variant_map (GFile *src,
                     const GVariantType *type,
                     GVariant **out_variant,
                     GError  **error)
{
  gboolean ret = FALSE;
  GMappedFile *mfile = NULL;
  char *path = NULL;
  GVariant *ret_variant = NULL;

  path = g_file_get_path (src);
  mfile = g_mapped_file_new (path, FALSE, error);
  if (!mfile)
    goto out;

  ret_variant = g_variant_new_from_data (type,
                                         g_mapped_file_get_contents (mfile),
                                         g_mapped_file_get_length (mfile),
                                         FALSE,
                                         (GDestroyNotify) g_mapped_file_unref,
                                         mfile);
  mfile = NULL;
  g_variant_ref_sink (ret_variant);
  
  ret = TRUE;
  *out_variant = ret_variant;
  ret_variant = NULL;
 out:
  if (ret_variant)
    g_variant_unref (ret_variant);
  if (mfile)
    g_mapped_file_unref (mfile);
  g_free (path);
  return ret;
}
