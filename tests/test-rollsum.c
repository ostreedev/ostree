/*
 * Copyright (C) 2015 Red Hat, Inc.
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
  g_autofree unsigned char *a = malloc (MAX_BUFFER_SIZE);
  g_autofree unsigned char *b = malloc (MAX_BUFFER_SIZE);
  g_autoptr(GRand) rand = g_rand_new ();

  /* These two buffers produce the same crc32.  */
  const unsigned char conflicting_a[] = {0x35, 0x9b, 0x94, 0x5a, 0xa0, 0x5a, 0x34, 0xdc, 0x5c, 0x3, 0x46, 0xe, 0x34, 0x53, 0x85, 0x73, 0x64, 0xcc, 0x47, 0x10, 0x23, 0x8e, 0x7e, 0x6a, 0xca, 0xda, 0x7c, 0x12, 0x8a, 0x59, 0x7f, 0x7f, 0x4d, 0x1, 0xd8, 0xcc, 0x81, 0xcf, 0x2c, 0x7f, 0x10, 0xc2, 0xb4, 0x40, 0x1f, 0x2a, 0x0, 0x37, 0x85, 0xde, 0xfe, 0xa5, 0xc, 0x7c, 0xa1, 0x8, 0xd6, 0x75, 0xfd, 0x2, 0xcf, 0x2d, 0x53, 0x1b, 0x8a, 0x6b, 0x35, 0xad, 0xa, 0x8f, 0xad, 0x2d, 0x91, 0x87, 0x2b, 0x97, 0xcf, 0x1d, 0x7c, 0x61, 0xc4, 0xb2, 0x5e, 0xc3, 0xba, 0x5d, 0x2f, 0x3a, 0xeb, 0x41, 0x61, 0x4c, 0xa2, 0x34, 0xd, 0x43, 0xce, 0x10, 0xa3, 0x47, 0x4, 0xa0, 0x39, 0x77, 0xc2, 0xe8, 0x36, 0x1d, 0x87, 0xd1, 0x8f, 0x4d, 0x13, 0xa1, 0x34, 0xc3, 0x2c, 0xee, 0x1a, 0x10, 0x79, 0xb7, 0x97, 0x29, 0xe8, 0xf0, 0x5, 0xfc, 0xe6, 0x14, 0x87, 0x9c, 0x8f, 0x97, 0x23, 0xac, 0x1, 0xf2, 0xee, 0x69, 0xb2, 0xe5};

  const unsigned char conflicting_b[] = {0xb2, 0x54, 0x81, 0x7d, 0x31, 0x83, 0xc7, 0xc, 0xcf, 0x7d, 0x90, 0x1c, 0x6b, 0xf6, 0x4e, 0xff, 0x49, 0xd1, 0xb6, 0xc, 0x9e, 0x85, 0xe3, 0x2d, 0xdb, 0x94, 0x8e, 0x1a, 0x17, 0x3f, 0x63, 0x59, 0xf9, 0x4b, 0x5f, 0x47, 0x97, 0x9c, 0x1c, 0xd7, 0x24, 0xd9, 0x42, 0x6, 0x1e, 0xf, 0x98, 0x10, 0xb4, 0xc, 0x50, 0xcb, 0xc5, 0x62, 0x53, 0x1, 0xd1, 0x5f, 0x16, 0x97, 0xaa, 0xd7, 0x57, 0x5e, 0xf2, 0xde, 0xae, 0x53, 0x58, 0x6, 0xb7, 0x9b, 0x8d, 0x2b, 0xd6, 0xb4, 0x55, 0x29, 0x3b, 0x27, 0x70, 0xd5, 0xf3, 0x8d, 0xdc, 0xad, 0x68, 0x63, 0xa5, 0x72, 0xce, 0x6b, 0x9, 0x2b, 0x60, 0x1b, 0x99, 0xd7, 0x86};

    test_rollsum_helper (conflicting_a, sizeof conflicting_a, conflicting_b, sizeof conflicting_b, FALSE);

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
  a[0] = g_rand_int (rand);
  b[0] = a[0] + 1;
  for (i = 1; i < MAX_BUFFER_SIZE; i++)
    {
      a[i] = g_rand_int (rand);
      b[i] = g_rand_int (rand);
    }
  test_rollsum_helper (a, MAX_BUFFER_SIZE, b, MAX_BUFFER_SIZE, FALSE);
}

#define BUP_SELFTEST_SIZE 100000

static void
test_bupsplit_sum(void)
{
    g_autofree uint8_t *buf = g_malloc (BUP_SELFTEST_SIZE);
    uint32_t sum1a, sum1b, sum2a, sum2b, sum3a, sum3b;
    unsigned count;

    for (count = 0; count < BUP_SELFTEST_SIZE; count++)
      buf[count] = g_random_int_range (0, 256);

    sum1a = bupsplit_sum(buf, 0, BUP_SELFTEST_SIZE);
    sum1b = bupsplit_sum(buf, 1, BUP_SELFTEST_SIZE);
    sum2a = bupsplit_sum(buf, BUP_SELFTEST_SIZE - BUP_WINDOWSIZE*5/2,
                         BUP_SELFTEST_SIZE - BUP_WINDOWSIZE);
    sum2b = bupsplit_sum(buf, 0, BUP_SELFTEST_SIZE - BUP_WINDOWSIZE);
    sum3a = bupsplit_sum(buf, 0, BUP_WINDOWSIZE+3);
    sum3b = bupsplit_sum(buf, 3, BUP_WINDOWSIZE+3);

    g_assert_cmpint (sum1a, ==, sum1b);
    g_assert_cmpint (sum2a, ==, sum2b);
    g_assert_cmpint (sum3a, ==, sum3b);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/rollsum", test_rollsum);
  g_test_add_func ("/bupsum", test_bupsplit_sum);
  return g_test_run();
}
