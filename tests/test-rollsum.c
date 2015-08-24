/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "libglnx.h"
#include "bsdiff/bsdiff.h"
#include "bsdiff/bspatch.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ostree-rollsum.h"
#include "bupsplit.h"

static void
test_rollsum_helper (const unsigned char *a, gsize size_a, const unsigned char *b, gsize size_b, gboolean expected_match)
{
  gsize i;
  g_autoptr(GBytes) bytes_a = g_bytes_new_static (a, size_a);
  g_autoptr(GBytes) bytes_b = g_bytes_new_static (b, size_b);
  OstreeRollsumMatches *matches;
  GPtrArray *matchlist;
  guint64 sum_matched = 0;

  matches = _ostree_compute_rollsum_matches (bytes_a, bytes_b);
  matchlist = matches->matches;
  if (expected_match)
    g_assert_cmpint (matchlist->len, >, 0);
  else
    g_assert_cmpint (matchlist->len, ==, 0);

  for (i = 0; i < matchlist->len; i++)
    {
      guint32 crc;
      GVariant *match = matchlist->pdata[i];
      guint64 offset = 0, to_start = 0, from_start = 0;
      g_variant_get (match, "(uttt)", &crc, &offset, &to_start, &from_start);

      g_assert_cmpint (offset, >=, 0);
      g_assert_cmpint (from_start, <, size_a);
      g_assert_cmpint (to_start, <, size_b);

      sum_matched += offset;

      g_assert_cmpint (memcmp (a + from_start, b + to_start, offset), ==, 0);
    }

  g_assert_cmpint (sum_matched, ==, matches->match_size);

  _ostree_rollsum_matches_free (matches);
}

static void
test_rollsum (void)
{
#define MAX_BUFFER_SIZE 1000000
  gsize i;
  int len;
  unsigned char *a = malloc (MAX_BUFFER_SIZE);
  unsigned char *b = malloc (MAX_BUFFER_SIZE);
  g_autoptr(GRand) rand = g_rand_new ();

  for (i = 0; i < MAX_BUFFER_SIZE; i++)
    {
      a[i] = g_rand_int (rand);
      b[i] = a[i];
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, TRUE);

  /* Do not overwrite the first buffer.  */
  len = bupsplit_find_ofs (b, MAX_BUFFER_SIZE, NULL);
  if (len)
    {
      unsigned char *ptr = b + len;
      gsize remaining = MAX_BUFFER_SIZE - len;
      while (remaining)
        {
          len = bupsplit_find_ofs (ptr, remaining, NULL);
          if (len == 0)
            break;
          *ptr = ~(*ptr);
          remaining -= len;
          ptr += len;
        }
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, TRUE);

  /* Duplicate the first buffer.  */
  len = bupsplit_find_ofs (b, MAX_BUFFER_SIZE, NULL);
  if (len && len < MAX_BUFFER_SIZE / 2)
    {
      memcpy (b + len, b, len);
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, TRUE);

  /* All different.  */
  for (i = 0; i < MAX_BUFFER_SIZE; i++)
    {
      a[i] = g_rand_int (rand);
      b[i] = a[i] + 1;
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, FALSE);

  /* All different.  */
  for (i = 0; i < MAX_BUFFER_SIZE; i++)
    {
      a[i] = g_rand_int (rand);
      b[i] = g_rand_int (rand);
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, FALSE);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/rollsum", test_rollsum);
  return g_test_run();
}
