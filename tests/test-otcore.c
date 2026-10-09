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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "otcore.h"

static void
test_ed25519 (void)
{
  g_autoptr (GBytes) empty = g_bytes_new_static ("", 0);
  bool valid = false;
  g_autoptr (GError) error = NULL;
  if (otcore_validate_ed25519_signature (empty, empty, empty, &valid, &error))
    g_assert_not_reached ();
  g_assert (error != NULL);
  g_clear_error (&error);
}

#ifdef HAVE_OPENSSL
/* An ECDSA P-256 public key in DER SubjectPublicKeyInfo form, as produced by
 *   openssl ecparam -name prime256v1 -genkey -noout |
 *     openssl ec -pubout -outform DER
 * The spki mechanism takes any key d2i_PUBKEY() understands.
 */
static const guint8 ec_p256_pubkey[]
    = { 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00,
        0x04, 0x80, 0x85, 0xe0, 0x67, 0xba, 0x0e, 0x92, 0xec, 0xa9, 0xf6, 0xfc, 0xa0,
        0x58, 0xc1, 0xda, 0x3f, 0x9b, 0xb3, 0x23, 0x59, 0x88, 0x93, 0x9d, 0x66, 0x36,
        0x30, 0x74, 0xb2, 0x7a, 0x9f, 0x4a, 0xf5, 0x53, 0x8d, 0x1b, 0xd8, 0xe6, 0xc7,
        0x82, 0x46, 0xa1, 0x65, 0x4d, 0x8a, 0x70, 0x2e, 0x71, 0xc3, 0x84, 0x8a, 0x20,
        0x3a, 0xc4, 0x18, 0x38, 0x9e, 0x24, 0x92, 0x99, 0x82, 0xcd, 0x80, 0x7f, 0xfd };

static void
test_spki (void)
{
  g_autoptr (GBytes) data = g_bytes_new_static ("some signed data", 16);
  g_autoptr (GBytes) pubkey = g_bytes_new_static (ec_p256_pubkey, sizeof (ec_p256_pubkey));
  /* Not a well-formed ECDSA signature, which openssl reports as an error
   * rather than as a signature mismatch.
   */
  g_autoptr (GBytes) signature = g_bytes_new_static ("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 32);
  bool valid = false;
  g_autoptr (GError) error = NULL;

  g_assert (otcore_validate_spki_signature (data, pubkey, signature, &valid, &error));
  g_assert_no_error (error);
  g_assert (!valid);
}
#endif

static void
test_prepare_root_cmdline (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *target = NULL;
  static const char *notfound_cases[]
      = { "", "foo", "foo=bar baz  sometest", "xostree foo", "xostree=blah bar", NULL };
  for (const char **iter = notfound_cases; iter && *iter; iter++)
    {
      const char *tcase = *iter;
      g_assert (otcore_get_ostree_target (tcase, NULL, &target, &error));
      g_assert_no_error (error);
      g_assert (target == NULL);
    }

  // Test the default ostree=
  g_assert (
      otcore_get_ostree_target ("blah baz=blah ostree=/foo/bar somearg", NULL, &target, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/foo/bar");
  free (g_steal_pointer (&target));

  // Test android boot
  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_b somearg", NULL,
                                      &target, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/ostree/root.b");
  free (g_steal_pointer (&target));

  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_a somearg", NULL,
                                      &target, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/ostree/root.a");
  free (g_steal_pointer (&target));

  // And an expected failure to parse a "c" suffix
  g_assert (!otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_c somearg", NULL,
                                       &target, &error));
  g_assert (error);
  g_assert (target == NULL);
  g_clear_error (&error);

  // And non-A/B androidboot
  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.somethingelse somearg", NULL,
                                      &target, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/ostree/root.a");
  free (g_steal_pointer (&target));
}

static void
test_prepare_root_config (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (GLnxTmpDir) tmpdir = {
    0,
  };
  g_assert (glnx_mkdtempat (AT_FDCWD, "/tmp/test-XXXXXX", 0777, &tmpdir, &error));
  g_assert_no_error (error);

  {
    g_autoptr (GKeyFile) config = NULL;
    g_auto (GStrv) keys = NULL;
    config = otcore_load_config (tmpdir.fd, "ostree/someconfig.conf", &error);
    g_assert (config);
    keys = g_key_file_get_groups (config, NULL);
    g_assert (keys && *keys == NULL);
  }

  g_assert (glnx_shutil_mkdir_p_at (tmpdir.fd, "usr/lib/ostree", 0755, NULL, NULL));
  g_assert (glnx_file_replace_contents_at (tmpdir.fd, "usr/lib/ostree/someconfig.conf",
                                           (guint8 *)"[foo]\nbar=baz", -1, 0, NULL, NULL));

  {
    g_autoptr (GKeyFile) config = NULL;
    g_auto (GStrv) keys = NULL;
    config = otcore_load_config (tmpdir.fd, "ostree/someconfig.conf", &error);
    g_assert (config);
    keys = g_key_file_get_groups (config, NULL);
    g_assert (keys);
    g_assert_cmpstr (*keys, ==, "foo");
  }

  g_assert (glnx_shutil_mkdir_p_at (tmpdir.fd, "etc/ostree", 0755, NULL, NULL));
  g_assert (glnx_file_replace_contents_at (tmpdir.fd, "usr/lib/ostree/someconfig.conf",
                                           (guint8 *)"[test]\nbar=baz", -1, 0, NULL, NULL));

  {
    g_autoptr (GKeyFile) config = NULL;
    g_auto (GStrv) keys = NULL;
    config = otcore_load_config (tmpdir.fd, "ostree/someconfig.conf", &error);
    g_assert (config);
    keys = g_key_file_get_groups (config, NULL);
    g_assert (keys);
    g_assert_cmpstr (*keys, ==, "test");
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  otcore_ed25519_init ();
  g_test_add_func ("/ed25519", test_ed25519);
#ifdef HAVE_OPENSSL
  otcore_spki_init ();
  g_test_add_func ("/spki", test_spki);
#endif
  g_test_add_func ("/prepare-root-cmdline", test_prepare_root_cmdline);
  g_test_add_func ("/prepare-root-config", test_prepare_root_config);
  return g_test_run ();
}
