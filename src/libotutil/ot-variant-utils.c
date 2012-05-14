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

#include <string.h>

#include "otutil.h"

GVariant *
ot_gvariant_new_bytearray (const guchar   *data,
                           gsize           len)
{
  gpointer data_copy;
  GVariant *ret;

  data_copy = g_memdup (data, len);
  ret = g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data_copy,
                                 len, FALSE, g_free, data_copy);
  return ret;
}

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
  ot_lobj GOutputStream *out = NULL;
  gsize bytes_written;
  
  out = (GOutputStream*)g_file_replace (dest, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                        cancellable, error);
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
  return ret;
}

GVariant *
ot_util_variant_take_ref (GVariant *variant)
{
#if GLIB_CHECK_VERSION(2,32,0) && !defined(OSTREE_GLIB_TARGET_MIN)
  return g_variant_take_ref (variant);
#else
  if (g_variant_is_floating (variant))
    return g_variant_ref_sink (variant);
  return variant;
#endif
}

/**
 * Return in @out_variant the result of memory-mapping the entire
 * contents of file @src.
 *
 * Note the returned @out_variant is not floating.
 */
gboolean
ot_util_variant_map (GFile              *src,
                     const GVariantType *type,
                     gboolean            trusted,
                     GVariant          **out_variant,
                     GError            **error)
{
  gboolean ret = FALSE;
  const char *path = NULL;
  ot_lvariant GVariant *ret_variant = NULL;
  GMappedFile *mfile = NULL;

  path = ot_gfile_get_path_cached (src);
  mfile = g_mapped_file_new (path, FALSE, error);
  if (!mfile)
    goto out;

  ret_variant = g_variant_new_from_data (type,
                                         g_mapped_file_get_contents (mfile),
                                         g_mapped_file_get_length (mfile),
                                         trusted,
                                         (GDestroyNotify) g_mapped_file_unref,
                                         mfile);
  mfile = NULL;
  g_variant_ref_sink (ret_variant);
  
  ret = TRUE;
  ot_transfer_out_value(out_variant, &ret_variant);
 out:
  if (mfile)
    g_mapped_file_unref (mfile);
  return ret;
}

/**
 * Read all input from @src, allocating a new #GVariant from it into
 * output variable @out_variant.  @src will be closed as a result.
 *
 * Note the returned @out_variant is not floating.
 */
gboolean
ot_util_variant_from_stream (GInputStream         *src,
                             const GVariantType   *type,
                             gboolean              trusted,
                             GVariant            **out_variant,
                             GCancellable         *cancellable,
                             GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GMemoryOutputStream *data_stream = NULL;
  ot_lvariant GVariant *ret_variant = NULL;

  data_stream = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);

  if (g_output_stream_splice ((GOutputStream*)data_stream, src,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    goto out;

  ret_variant = g_variant_new_from_data (type, g_memory_output_stream_get_data (data_stream),
                                         g_memory_output_stream_get_data_size (data_stream),
                                         trusted, (GDestroyNotify) g_object_unref, data_stream);
  data_stream = NULL; /* Transfer ownership */
  g_variant_ref_sink (ret_variant);

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

GInputStream *
ot_variant_read (GVariant             *variant)
{
  GMemoryInputStream *ret = NULL;

  ret = (GMemoryInputStream*)g_memory_input_stream_new_from_data (g_variant_get_data (variant),
                                                                  g_variant_get_size (variant),
                                                                  NULL);
  g_object_set_data_full ((GObject*)ret, "ot-variant-data",
                          g_variant_ref (variant), (GDestroyNotify) g_variant_unref);
  return (GInputStream*)ret;
}

