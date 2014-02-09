/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "libgsystem.h"

#include "bupsplit.h"

#define BLOB_MAX (8192*4)

static GPtrArray *
rollsum_checksums_for_data (GBytes     *bytes)
{
  const guint8 *start;
  gsize len;
  GPtrArray *ret = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  start = g_bytes_get_data (bytes, &len);
  while (TRUE)
    {
      int offset, bits;
      offset = bupsplit_find_ofs (start, MIN(G_MAXINT32, len), &bits); 
      if (offset == 0)
        break;
      if (offset > BLOB_MAX)
        offset = BLOB_MAX;
      {
        gs_free char *blobcsum =
          g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                       start, offset);
        g_ptr_array_add (ret, g_variant_ref_sink (g_variant_new ("(st)",
                                                                 blobcsum, (guint64)offset)));
      }
      start += offset;
      len -= offset;
    }
  return ret;
}

static void
print_rollsums (GPtrArray  *rollsums)
{
  guint i;
  for (i = 0; i < rollsums->len; i++)
    {
      GVariant *sum = rollsums->pdata[i];
      const char *csum;
      guint64 val;
      g_variant_get (sum, "(&st)", &csum, &val);
      g_print ("chunk %s %" G_GUINT64_FORMAT "\n", csum, val);
    }
}

int
main (int argc, char **argv)
{
  GCancellable *cancellable = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_unref_object GFile *path = NULL;
  GBytes *bytes = NULL;

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (argc == 2)
    {
      gs_unref_ptrarray GPtrArray *rollsums = NULL;

      path = g_file_new_for_path (argv[1]);
      bytes = gs_file_map_readonly (path, cancellable, error);
      if (!bytes)
	goto out;

      rollsums = rollsum_checksums_for_data (bytes);
      print_rollsums (rollsums);
    }
  else if (argc > 2)
    {
      guint i;
      gs_unref_hashtable GHashTable *sums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      guint64 input_size = 0;
      guint64 rollsum_size = 0;

      for (i = 1; i < argc; i++)
        {
          guint j;
          gs_unref_ptrarray GPtrArray *rollsums = NULL;

          path = g_file_new_for_path (argv[i]);
          bytes = gs_file_map_readonly (path, cancellable, error);
          if (!bytes)
            goto out;

          input_size += g_bytes_get_size (bytes);
          
          g_print ("input: %s size: %" G_GUINT64_FORMAT "\n", argv[i], g_bytes_get_size (bytes));

          rollsums = rollsum_checksums_for_data (bytes);
          print_rollsums (rollsums);
          for (j = 0; j < rollsums->len; j++)
            {
              GVariant *sum = rollsums->pdata[j];
              const char *csum;
              guint64 ofs;
              g_variant_get (sum, "(&st)", &csum, &ofs);
              if (!g_hash_table_contains (sums, csum))
                {
                  g_hash_table_add (sums, g_strdup (csum));
                  rollsum_size += ofs;
                }
            }
        }
      g_print ("rollsums:%u input:%" G_GUINT64_FORMAT " output: %" G_GUINT64_FORMAT " speedup:%f\n",
               g_hash_table_size (sums), input_size, rollsum_size,
               (((double)(input_size+1)) / ((double) rollsum_size + 1)));
    }
  else
    {
      bupsplit_selftest ();
    }

 out:
  g_clear_pointer (&bytes, g_bytes_unref);
  if (local_error)
    {
      g_printerr ("%s\n", local_error->message);
      g_error_free (local_error);
      return 1;
    }
  return 0;
}
