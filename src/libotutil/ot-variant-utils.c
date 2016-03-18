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
#include <gio/gfiledescriptorbased.h>

#include <string.h>
#include <sys/mman.h>

#include "otutil.h"

GVariant *
ot_gvariant_new_empty_string_dict (void)
{
  return g_variant_builder_end (g_variant_builder_new (G_VARIANT_TYPE ("a{sv}")));
}

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

GVariant *
ot_gvariant_new_ay_bytes (GBytes *bytes)
{
  gsize size;
  gconstpointer data;
  data = g_bytes_get_data (bytes, &size);
  g_bytes_ref (bytes);
  return g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data, size,
                                  TRUE, (GDestroyNotify)g_bytes_unref, bytes);
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
  g_autoptr(GOutputStream) out = NULL;
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
  return g_variant_take_ref (variant);
}

/**
 * ot_util_variant_map:
 * @src: a #GFile
 * @type: Use this for variant
 * @trusted: See documentation of g_variant_new_from_data()
 * @out_variant: (out): Return location for new variant
 * @error:
 *
 * Memory-map @src, and store a new #GVariant referring to this memory
 * in @out_variant.  Note the returned @out_variant is not floating.
 */
gboolean
ot_util_variant_map (GFile              *src,
                     const GVariantType *type,
                     gboolean            trusted,
                     GVariant          **out_variant,
                     GError            **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) ret_variant = NULL;
  GMappedFile *mfile = NULL;

  mfile = gs_file_map_noatime (src, NULL, error);
  if (!mfile)
    goto out;

  ret_variant = g_variant_new_from_data (type,
                                         g_mapped_file_get_contents (mfile),
                                         g_mapped_file_get_length (mfile),
                                         trusted,
                                         (GDestroyNotify) g_mapped_file_unref,
                                         mfile);
  g_variant_ref_sink (ret_variant);
  
  ret = TRUE;
  ot_transfer_out_value(out_variant, &ret_variant);
 out:
  return ret;
}

gboolean
ot_util_variant_map_at (int dfd,
                        const char *path,
                        const GVariantType *type,
                        gboolean trusted,
                        GVariant **out_variant,
                        GError  **error)
{
  glnx_fd_close int fd = -1;
  g_autoptr(GVariant) ret_variant = NULL;

  fd = openat (dfd, path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Opening %s: ", path);
      return FALSE;
    }

  return ot_util_variant_map_fd (fd, 0, type, trusted, out_variant, error);
}

typedef struct {
  gpointer addr;
  gsize len;
} VariantMapData;

static void
variant_map_data_destroy (gpointer data)
{
  VariantMapData *mdata = data;
  (void) munmap (mdata->addr, mdata->len);
}

gboolean
ot_util_variant_map_fd (int                    fd,
                        goffset                start,
                        const GVariantType    *type,
                        gboolean               trusted,
                        GVariant             **out_variant,
                        GError               **error)
{
  gboolean ret = FALSE;
  gpointer map;
  struct stat stbuf;
  VariantMapData *mdata = NULL;
  gsize len;

  if (fstat (fd, &stbuf) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  len = stbuf.st_size - start;
  map = mmap (NULL, len, PROT_READ, MAP_PRIVATE, fd, start);
  if (!map)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  mdata = g_new (VariantMapData, 1);
  mdata->addr = map;
  mdata->len = len;

  ret = TRUE;
  *out_variant = g_variant_new_from_data (type, map, len, trusted,
                                          variant_map_data_destroy, mdata);
 out:
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
  g_autoptr(GMemoryOutputStream) data_stream = NULL;
  g_autoptr(GVariant) ret_variant = NULL;

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

GVariantBuilder *
ot_util_variant_builder_from_variant (GVariant            *variant,
                                      const GVariantType  *type)
{
  GVariantBuilder *builder = NULL;
  
  builder = g_variant_builder_new (type);
  
  if (variant != NULL)
    {
      gint i, n;

      n = g_variant_n_children (variant);
      for (i = 0; i < n; i++)
        {
          GVariant *child = g_variant_get_child_value (variant, i);
          g_variant_builder_add_value (builder, child);
          g_variant_unref (child);
        }
    }
    
  return builder;
}

/**
 * ot_variant_bsearch_str:
 * @array: A GVariant array whose first element must be a string
 * @str: Search for this string
 * @out_pos: Output position
 *
 *
 * Binary search in a GVariant array, which must be of the form 'a(s...)',
 * where '...' may be anything.  The array elements must be sorted.
 *
 * Returns: %TRUE if found, %FALSE otherwise
 */
gboolean
ot_variant_bsearch_str (GVariant   *array,
                        const char *str,
                        int        *out_pos)
{
  gsize imax, imin;
  gsize imid;
  gsize n;

  n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      g_autoptr(GVariant) child = NULL;
      const char *cur;
      int cmp;

      imid = (imin + imax) / 2;

      child = g_variant_get_child_value (array, imid);
      g_variant_get_child (child, 0, "&s", &cur, NULL);      

      cmp = strcmp (cur, str);
      if (cmp < 0)
        imin = imid + 1;
      else if (cmp > 0)
        {
          if (imid == 0)
            break;
          imax = imid - 1;
        }
      else
        {
          *out_pos = imid;
          return TRUE;
        }
    }

  *out_pos = imid;
  return FALSE;
}
