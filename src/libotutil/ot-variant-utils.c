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

#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>

#include <string.h>

#include "otutil.h"

/* Create a new GVariant empty GVariant of type a{sv} */
GVariant *
ot_gvariant_new_empty_string_dict (void)
{
  g_auto(GVariantBuilder) builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  return g_variant_builder_end (&builder);
}


/* Create a new GVariant of type ay from the raw @data pointer */
GVariant *
ot_gvariant_new_bytearray (const guchar   *data,
                           gsize           len)
{
  gpointer data_copy = g_memdup (data, len);
  GVariant *ret = g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data_copy,
                                 len, FALSE, g_free, data_copy);
  return ret;
}

/* Convert a GBytes into a GVariant of type ay (byte array) */
GVariant *
ot_gvariant_new_ay_bytes (GBytes *bytes)
{
  gsize size;
  gconstpointer data = g_bytes_get_data (bytes, &size);
  g_bytes_ref (bytes);
  return g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data, size,
                                  TRUE, (GDestroyNotify)g_bytes_unref, bytes);
}

/* Create a GVariant in @out_variant that is backed by
 * the data from @fd, starting at @start.  If the data is
 * large enough, mmap() may be used.  @trusted is used
 * by the GVariant core; see g_variant_new_from_data().
 */
gboolean
ot_variant_read_fd (int                    fd,
                    goffset                start,
                    const GVariantType    *type,
                    gboolean               trusted,
                    GVariant             **out_variant,
                    GError               **error)
{
  g_autoptr(GBytes) bytes = ot_fd_readall_or_mmap (fd, start, error);
  if (!bytes)
    return FALSE;

  *out_variant = g_variant_ref_sink (g_variant_new_from_bytes (type, bytes, trusted));
  return TRUE;
}

/* GVariants are immutable; this function allows generating an open builder
 * for a new variant, inherting the data from @variant.
 */
GVariantBuilder *
ot_util_variant_builder_from_variant (GVariant            *variant,
                                      const GVariantType  *type)
{
  GVariantBuilder *builder = g_variant_builder_new (type);

  if (variant != NULL)
    {
      const int n = g_variant_n_children (variant);
      for (int i = 0; i < n; i++)
        {
          g_autoptr(GVariant) child = g_variant_get_child_value (variant, i);
          g_variant_builder_add_value (builder, child);
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
  const gsize n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  gsize imax = n - 1;
  gsize imin = 0;
  gsize imid = -1;
  while (imax >= imin)
    {
      const char *cur;

      imid = (imin + imax) / 2;

      g_autoptr(GVariant) child = g_variant_get_child_value (array, imid);
      g_variant_get_child (child, 0, "&s", &cur, NULL);

      int cmp = strcmp (cur, str);
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
