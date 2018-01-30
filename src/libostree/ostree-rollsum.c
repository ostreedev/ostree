/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

#include <string.h>
#include <zlib.h>

#include "ostree-rollsum.h"
#include "libglnx.h"
#include "bupsplit.h"

#define ROLLSUM_BLOB_MAX (8192*4)

static GHashTable *
rollsum_chunks_crc32 (GBytes           *bytes)
{
  gsize start = 0;
  gboolean rollsum_end = FALSE;
  GHashTable *ret_rollsums = NULL;
  const guint8 *buf;
  gsize buflen;
  gsize remaining;

  ret_rollsums = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  buf = g_bytes_get_data (bytes, &buflen);

  remaining = buflen;
  while (remaining > 0)
    {
      int offset, bits;

      if (!rollsum_end)
        {
          offset = bupsplit_find_ofs (buf + start, MIN(G_MAXINT32, remaining), &bits); 
          if (offset == 0)
            {
              rollsum_end = TRUE;
              offset = MIN(ROLLSUM_BLOB_MAX, remaining);
            }
          else if (offset > ROLLSUM_BLOB_MAX)
            offset = ROLLSUM_BLOB_MAX;
        }
      else
        offset = MIN(ROLLSUM_BLOB_MAX, remaining);

      /* Use zlib's crc32 */
      { guint32 crc = crc32 (0L, NULL, 0);
        GVariant *val;
        GPtrArray *matches;

        crc = crc32 (crc, buf, offset);

        val = g_variant_ref_sink (g_variant_new ("(utt)", crc, (guint64) start, (guint64)offset));
        matches = g_hash_table_lookup (ret_rollsums, GUINT_TO_POINTER (crc));
        if (!matches)
          {
            matches = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
            g_hash_table_insert (ret_rollsums, GUINT_TO_POINTER (crc), matches);
          }
        g_ptr_array_add (matches, val);
      }

      start += offset;
      remaining -= offset;
    }

  return ret_rollsums;
}

static gint
compare_matches (const void *app,
                 const void *bpp)
{
  GVariant **avpp = (GVariant**)app;
  GVariant *a = *avpp;
  GVariant **bvpp = (GVariant**)bpp;
  GVariant *b = *bvpp;
  guint64 a_start, b_start;
  
  g_variant_get_child (a, 2, "t", &a_start);
  g_variant_get_child (b, 2, "t", &b_start);

  g_assert_cmpint (a_start, !=, b_start);

  if (a_start < b_start)
    return -1;
  return 1;
}

OstreeRollsumMatches *
_ostree_compute_rollsum_matches (GBytes                           *from,
                                 GBytes                           *to)
{
  OstreeRollsumMatches *ret_rollsum = NULL;
  g_autoptr(GHashTable) from_rollsum = NULL;
  g_autoptr(GHashTable) to_rollsum = NULL;
  g_autoptr(GPtrArray) matches = NULL;
  const guint8 *from_buf;
  gsize from_len;
  const guint8 *to_buf;
  gsize to_len;
  gpointer hkey, hvalue;
  GHashTableIter hiter;

  ret_rollsum = g_new0 (OstreeRollsumMatches, 1);

  matches = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  from_buf = g_bytes_get_data (from, &from_len);
  to_buf = g_bytes_get_data (to, &to_len);

  from_rollsum = rollsum_chunks_crc32 (from);
  to_rollsum = rollsum_chunks_crc32 (to);

  g_hash_table_iter_init (&hiter, to_rollsum);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      GPtrArray *to_chunks = hvalue;
      GPtrArray *from_chunks;

      from_chunks = g_hash_table_lookup (from_rollsum, hkey);
      if (from_chunks != NULL)
        {
          guint i;

          ret_rollsum->crcmatches++;

          for (i = 0; i < to_chunks->len; i++)
            {
              GVariant *to_chunk = to_chunks->pdata[i];
              guint64 to_start, to_offset;
              guint32 tocrc;
              guint j;

              g_variant_get (to_chunk, "(utt)", &tocrc, &to_start, &to_offset);

              for (j = 0; j < from_chunks->len; j++)
                {
                  GVariant *from_chunk = from_chunks->pdata[j];
                  guint32 fromcrc;
                  guint64 from_start, from_offset;

                  g_variant_get (from_chunk, "(utt)", &fromcrc, &from_start, &from_offset);

                  g_assert (fromcrc == tocrc);

                  /* Same crc32 but different length, skip it.  */
                  if (to_offset != from_offset)
                    continue;
                  
                  /* Rsync uses a cryptographic checksum, but let's be
                   * very conservative here and just memcmp.
                   */
                  if (memcmp (from_buf + from_start, to_buf + to_start, to_offset) == 0)
                    {
                      GVariant *match = g_variant_new ("(uttt)", fromcrc, to_offset, to_start, from_start);
                      ret_rollsum->bufmatches++;
                      ret_rollsum->match_size += to_offset;
                      g_ptr_array_add (matches, g_variant_ref_sink (match));
                      break; /* Don't need any more matches */
                    } 
                }
            }
        }

      ret_rollsum->total += to_chunks->len;
    }

  g_ptr_array_sort (matches, compare_matches);

  ret_rollsum->from_rollsums = from_rollsum; from_rollsum = NULL;
  ret_rollsum->to_rollsums = to_rollsum; to_rollsum = NULL;
  ret_rollsum->matches = matches; matches = NULL;

  return ret_rollsum;
}

void
_ostree_rollsum_matches_free (OstreeRollsumMatches *rollsum)
{
  g_hash_table_unref (rollsum->to_rollsums);
  g_hash_table_unref (rollsum->from_rollsums);
  g_ptr_array_unref (rollsum->matches);
  g_free (rollsum);
}
