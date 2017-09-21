/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>

#include "ostree-bloom-private.h"

/* Test the two different constructors work at a basic level. */
static void
test_bloom_init (void)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  g_autoptr(GBytes) bytes = NULL;

  bloom = ostree_bloom_new (1, 1, ostree_str_bloom_hash);
  g_assert_cmpuint (ostree_bloom_get_size (bloom), ==, 1);
  g_assert_cmpuint (ostree_bloom_get_k (bloom), ==, 1);
  g_assert (ostree_bloom_get_hash_func (bloom) == ostree_str_bloom_hash);
  g_clear_pointer (&bloom, ostree_bloom_unref);

  bytes = g_bytes_new_take (g_malloc0 (4), 4);
  bloom = ostree_bloom_new_from_bytes (bytes, 1, ostree_str_bloom_hash);
  g_assert_cmpuint (ostree_bloom_get_size (bloom), ==, 4);
  g_assert_cmpuint (ostree_bloom_get_k (bloom), ==, 1);
  g_assert (ostree_bloom_get_hash_func (bloom) == ostree_str_bloom_hash);
  g_clear_pointer (&bloom, ostree_bloom_unref);
}

/* Test that building a bloom filter, marshalling it through GBytes, and loading
 * it again, gives the same element membership. */
static void
test_bloom_construction (void)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  g_autoptr(OstreeBloom) immutable_bloom = NULL;
  g_autoptr(GBytes) bytes = NULL;
  gsize i;
  const gchar *members[] =
    {
      "hello", "there", "these", "are", "test", "strings"
    };
  const gchar *non_members[] =
    {
      "not", "an", "element"
    };
  const gsize n_bytes = 256;
  const guint8 k = 8;
  const OstreeBloomHashFunc hash = ostree_str_bloom_hash;

  /* Build a bloom filter. */
  bloom = ostree_bloom_new (n_bytes, k, hash);

  for (i = 0; i < G_N_ELEMENTS (members); i++)
    ostree_bloom_add_element (bloom, members[i]);

  bytes = ostree_bloom_seal (bloom);

  /* Read it back from the GBytes. */
  immutable_bloom = ostree_bloom_new_from_bytes (bytes, k, hash);

  for (i = 0; i < G_N_ELEMENTS (members); i++)
    g_assert_true (ostree_bloom_maybe_contains (bloom, members[i]));

  /* This should never fail in future, as we guarantee the hash function will
   * never change. But given the definition of a bloom filter, it would also
   * be valid for these calls to return %TRUE. */
  for (i = 0; i < G_N_ELEMENTS (non_members); i++)
    g_assert_false (ostree_bloom_maybe_contains (bloom, non_members[i]));
}

/* Test that an empty bloom filter definitely contains no elements. */
static void
test_bloom_empty (void)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  const gsize n_bytes = 256;
  const guint8 k = 8;
  const OstreeBloomHashFunc hash = ostree_str_bloom_hash;

  /* Build an empty bloom filter. */
  bloom = ostree_bloom_new (n_bytes, k, hash);

  g_assert_false (ostree_bloom_maybe_contains (bloom, "hello"));
  g_assert_false (ostree_bloom_maybe_contains (bloom, "there"));
}

/* Build a bloom filter, and check the membership of the members as they are
 * added. */
static void
test_bloom_membership_during_construction (void)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  gsize i, j;
  const gchar *members[] =
    {
      "hello", "there", "these", "are", "test", "strings"
    };
  const gsize n_bytes = 256;
  const guint8 k = 8;
  const OstreeBloomHashFunc hash = ostree_str_bloom_hash;

  /* These membership checks should never fail in future, as we guarantee
   * the hash function will never change. But given the definition of a bloom
   * filter, it would also be valid for these checks to fail. */
  bloom = ostree_bloom_new (n_bytes, k, hash);

  for (i = 0; i < G_N_ELEMENTS (members); i++)
    {
      ostree_bloom_add_element (bloom, members[i]);

      for (j = 0; j < G_N_ELEMENTS (members); j++)
        {
          if (j <= i)
            g_assert_true (ostree_bloom_maybe_contains (bloom, members[j]));
          else
            g_assert_false (ostree_bloom_maybe_contains (bloom, members[j]));
        }
    }
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/bloom/init", test_bloom_init);
  g_test_add_func ("/bloom/construction", test_bloom_construction);
  g_test_add_func ("/bloom/empty", test_bloom_empty);
  g_test_add_func ("/bloom/membership-during-construction", test_bloom_membership_during_construction);

  return g_test_run();
}
