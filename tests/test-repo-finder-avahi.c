/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
#include <glib-object.h>
#include <locale.h>
#include <string.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-avahi.h"
#include "ostree-repo-finder-avahi-private.h"

/* FIXME: Upstream this */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AvahiStringList, avahi_string_list_free)

/* Test the object constructor works at a basic level. */
static void
test_repo_finder_avahi_init (void)
{
  g_autoptr(OstreeRepoFinderAvahi) finder = NULL;
  g_autoptr(GMainContext) context = NULL;

  /* Default main context. */
  finder = ostree_repo_finder_avahi_new (NULL);
  g_clear_object (&finder);

  /* Explicit main context. */
  context = g_main_context_new ();
  finder = ostree_repo_finder_avahi_new (context);
  g_clear_object (&finder);
}

/* Test parsing valid and invalid TXT records. */
static void
test_repo_finder_avahi_txt_records_parse (void)
{
  struct
    {
      const guint8 *txt;
      gsize txt_len;
      const gchar *expected_key;  /* (nullable) to indicate parse failure */
      const guint8 *expected_value;  /* (nullable) to allow for valueless keys */
      gsize expected_value_len;
    }
  vectors[] =
    {
      { (const guint8 *) "", 0, NULL, NULL, 0 },
      { (const guint8 *) "\x00", 1, NULL, NULL, 0 },
      { (const guint8 *) "\xff", 1, NULL, NULL, 0 },
      { (const guint8 *) "k\x00", 2, NULL, NULL, 0 },
      { (const guint8 *) "k\xff", 2, NULL, NULL, 0 },
      { (const guint8 *) "=", 1, NULL, NULL, 0 },
      { (const guint8 *) "=value", 6, NULL, NULL, 0 },
      { (const guint8 *) "k=v", 3, "k", (const guint8 *) "v", 1 },
      { (const guint8 *) "key=value", 9, "key", (const guint8 *) "value", 5 },
      { (const guint8 *) "k=v=", 4, "k", (const guint8 *) "v=", 2 },
      { (const guint8 *) "k=", 2, "k", (const guint8 *) "", 0 },
      { (const guint8 *) "k", 1, "k", NULL, 0 },
      { (const guint8 *) "k==", 3, "k", (const guint8 *) "=", 1 },
      { (const guint8 *) "k=\x00\x01\x02", 5, "k", (const guint8 *) "\x00\x01\x02", 3 },
    };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (vectors); i++)
    {
      g_autoptr(AvahiStringList) string_list = NULL;
      g_autoptr(GHashTable) attributes = NULL;

      g_test_message ("Vector %" G_GSIZE_FORMAT, i);

      string_list = avahi_string_list_add_arbitrary (NULL, vectors[i].txt, vectors[i].txt_len);

      attributes = _ostree_txt_records_parse (string_list);

      if (vectors[i].expected_key != NULL)
        {
          GBytes *value;
          g_autoptr(GBytes) expected_value = NULL;

          g_assert_true (g_hash_table_lookup_extended (attributes,
                                                       vectors[i].expected_key,
                                                       NULL,
                                                       (gpointer *) &value));
          g_assert_cmpuint (g_hash_table_size (attributes), ==, 1);

          if (vectors[i].expected_value != NULL)
            {
              g_assert_nonnull (value);
              expected_value = g_bytes_new_static (vectors[i].expected_value, vectors[i].expected_value_len);
              g_assert_true (g_bytes_equal (value, expected_value));
            }
          else
            {
              g_assert_null (value);
            }
        }
      else
        {
          g_assert_cmpuint (g_hash_table_size (attributes), ==, 0);
        }
    }
}

/* Test that the first value for a set of duplicate records is returned.
 * See RFC 6763, §6.4. */
static void
test_repo_finder_avahi_txt_records_duplicates (void)
{
  g_autoptr(AvahiStringList) string_list = NULL;
  g_autoptr(GHashTable) attributes = NULL;
  GBytes *value;
  g_autoptr(GBytes) expected_value = NULL;

  /* Reverse the list before using it, as they are built in reverse order.
   * (See the #AvahiStringList documentation.) */
  string_list = avahi_string_list_new ("k=value1", "k=value2", "k=value3", NULL);
  string_list = avahi_string_list_reverse (string_list);
  attributes = _ostree_txt_records_parse (string_list);

  g_assert_cmpuint (g_hash_table_size (attributes), ==, 1);
  value = g_hash_table_lookup (attributes, "k");
  g_assert_nonnull (value);

  expected_value = g_bytes_new_static ("value1", strlen ("value1"));
  g_assert_true (g_bytes_equal (value, expected_value));
}

/* Test that keys are parsed and looked up case insensitively.
 * See RFC 6763, §6.4. */
static void
test_repo_finder_avahi_txt_records_case_sensitivity (void)
{
  g_autoptr(AvahiStringList) string_list = NULL;
  g_autoptr(GHashTable) attributes = NULL;
  GBytes *value1, *value2;
  g_autoptr(GBytes) expected_value1 = NULL, expected_value2 = NULL;

  /* Reverse the list before using it, as they are built in reverse order.
   * (See the #AvahiStringList documentation.) */
  string_list = avahi_string_list_new ("k=value1",
                                       "K=value2",
                                       "KeY2=v",
                                       NULL);
  string_list = avahi_string_list_reverse (string_list);
  attributes = _ostree_txt_records_parse (string_list);

  g_assert_cmpuint (g_hash_table_size (attributes), ==, 2);

  value1 = g_hash_table_lookup (attributes, "k");
  g_assert_nonnull (value1);
  expected_value1 = g_bytes_new_static ("value1", strlen ("value1"));
  g_assert_true (g_bytes_equal (value1, expected_value1));

  g_assert_null (g_hash_table_lookup (attributes, "K"));

  value2 = g_hash_table_lookup (attributes, "key2");
  g_assert_nonnull (value2);
  expected_value2 = g_bytes_new_static ("v", 1);
  g_assert_true (g_bytes_equal (value2, expected_value2));

  g_assert_null (g_hash_table_lookup (attributes, "KeY2"));
}

/* Test that keys which have an empty value can be distinguished from those
 * which have no value. See RFC 6763, §6.4. */
static void
test_repo_finder_avahi_txt_records_empty_and_missing (void)
{
  g_autoptr(AvahiStringList) string_list = NULL;
  g_autoptr(GHashTable) attributes = NULL;
  GBytes *value1, *value2;
  g_autoptr(GBytes) expected_value1 = NULL;

  string_list = avahi_string_list_new ("empty=",
                                       "missing",
                                       NULL);
  attributes = _ostree_txt_records_parse (string_list);

  g_assert_cmpuint (g_hash_table_size (attributes), ==, 2);

  value1 = g_hash_table_lookup (attributes, "empty");
  g_assert_nonnull (value1);
  expected_value1 = g_bytes_new_static ("", 0);
  g_assert_true (g_bytes_equal (value1, expected_value1));

  g_assert_true (g_hash_table_lookup_extended (attributes, "missing", NULL, (gpointer *) &value2));
  g_assert_null (value2);
}

int main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/repo-finder-avahi/init", test_repo_finder_avahi_init);
  g_test_add_func ("/repo-finder-avahi/txt-records/parse", test_repo_finder_avahi_txt_records_parse);
  g_test_add_func ("/repo-finder-avahi/txt-records/duplicates", test_repo_finder_avahi_txt_records_duplicates);
  g_test_add_func ("/repo-finder-avahi/txt-records/case-sensitivity", test_repo_finder_avahi_txt_records_case_sensitivity);
  g_test_add_func ("/repo-finder-avahi/txt-records/empty-and-missing", test_repo_finder_avahi_txt_records_empty_and_missing);
  /* FIXME: Add tests for service processing, probably by splitting the
   * code in OstreeRepoFinderAvahi around found_services. */

  return g_test_run();
}
