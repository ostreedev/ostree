/*
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gio/gio.h>

#include "ostree-blob-reader-private.h"

static const guint8 pubkey_ed25519[]
    = { 0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00, 0x36, 0x09, 0x06,
        0x69, 0xf3, 0x52, 0xb1, 0xe3, 0x7e, 0xd4, 0xb5, 0xe3, 0x4c, 0x52, 0x6b, 0x7d, 0xdb, 0xba,
        0x37, 0x6a, 0xac, 0xe6, 0xb9, 0x5f, 0xf5, 0xdd, 0xf1, 0x95, 0xa5, 0x5c, 0x96, 0x09 };

static const gchar pem_pubkey_ed25519[]
    = "-----BEGIN PUBLIC KEY-----\n"
      "MCowBQYDK2VwAyEANgkGafNSseN+1LXjTFJrfdu6N2qs5rlf9d3xlaVclgk=\n"
      "-----END PUBLIC KEY-----\n";

static const gchar pem_pubkey_ed25519_whitespace[]
    = "-----BEGIN PUBLIC KEY-----\n"
      " \n"
      "MCowBQYDK2VwAyEANgkGafNSseN+1LXjTFJrfdu6N2qs5rlf9d3xlaVclgk=\n"
      "-----END PUBLIC KEY-----\n";

static const gchar pem_pubkey_empty[] = "";

static const gchar pem_pubkey_ed25519_no_trailer[]
    = "-----BEGIN PUBLIC KEY-----\n"
      "MCowBQYDK2VwAyEANgkGafNSseN+1LXjTFJrfdu6N2qs5rlf9d3xlaVclgk=\n";

static const gchar pem_pubkey_ed25519_label_mismatch[]
    = "-----BEGIN PUBLIC KEY X-----\n"
      "MCowBQYDK2VwAyEANgkGafNSseN+1LXjTFJrfdu6N2qs5rlf9d3xlaVclgk=\n"
      "-----END PUBLIC KEY Y-----\n";

static void
test_ostree_read_pem_block_valid (void)
{
  static const struct
  {
    const gchar *pem_data;
    gsize pem_size;
    const gchar *label;
    const guint8 *data;
    gsize size;
  } tests[] = {
    { pem_pubkey_ed25519, sizeof (pem_pubkey_ed25519), "PUBLIC KEY", pubkey_ed25519,
      sizeof (pubkey_ed25519) },
    { pem_pubkey_ed25519_whitespace, sizeof (pem_pubkey_ed25519_whitespace), "PUBLIC KEY",
      pubkey_ed25519, sizeof (pubkey_ed25519) },
    { pem_pubkey_empty, sizeof (pem_pubkey_empty), NULL, NULL, 0 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_autoptr (GInputStream) stream
          = g_memory_input_stream_new_from_data (tests[i].pem_data, tests[i].pem_size, NULL);
      g_autoptr (GDataInputStream) data_stream = g_data_input_stream_new (stream);

      g_autofree char *label = NULL;
      g_autoptr (GBytes) bytes = NULL;
      g_autoptr (GError) error = NULL;
      bytes = _ostree_read_pem_block (data_stream, &label, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpstr (label, ==, tests[i].label);
      if (tests[i].data)
        {
          g_autoptr (GBytes) expected_bytes = g_bytes_new_static (tests[i].data, tests[i].size);
          g_assert (g_bytes_equal (bytes, expected_bytes));
        }
    }
}

static void
test_ostree_read_pem_block_invalid (void)
{
  static const struct
  {
    const gchar *pem_data;
    gsize pem_size;
  } tests[] = {
    { pem_pubkey_ed25519_no_trailer, sizeof (pem_pubkey_ed25519_no_trailer) },
    { pem_pubkey_ed25519_label_mismatch, sizeof (pem_pubkey_ed25519_label_mismatch) },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_autoptr (GInputStream) stream
          = g_memory_input_stream_new_from_data (tests[i].pem_data, tests[i].pem_size, NULL);
      g_autoptr (GDataInputStream) data_stream = g_data_input_stream_new (stream);

      g_autofree char *label = NULL;
      g_autoptr (GBytes) bytes = NULL;
      g_autoptr (GError) error = NULL;
      bytes = _ostree_read_pem_block (data_stream, &label, NULL, &error);
      g_assert_null (bytes);
      g_assert_null (label);
      g_assert_nonnull (error);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ostree_read_pem_block/valid", test_ostree_read_pem_block_valid);
  g_test_add_func ("/ostree_read_pem_block/invalid", test_ostree_read_pem_block_invalid);
  return g_test_run ();
}
