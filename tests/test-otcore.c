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
      g_assert (otcore_get_ostree_target (tcase, &target, &error));
      g_assert_no_error (error);
      g_assert (target == NULL);
    }

  // Test the default ostree=
  g_assert (otcore_get_ostree_target ("blah baz=blah ostree=/foo/bar somearg", &target, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/foo/bar");
  free (g_steal_pointer (&target));

  // Test android boot
  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_b somearg", &target,
                                      &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/ostree/root.b");
  free (g_steal_pointer (&target));

  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_a somearg", &target,
                                      &error));
  g_assert_no_error (error);
  g_assert_cmpstr (target, ==, "/ostree/root.a");
  free (g_steal_pointer (&target));

  // And an expected failure to parse a "c" suffix
  g_assert (!otcore_get_ostree_target ("blah baz=blah androidboot.slot_suffix=_c somearg", &target,
                                       &error));
  g_assert (error);
  g_assert (target == NULL);
  g_clear_error (&error);

  // And non-A/B androidboot
  g_assert (otcore_get_ostree_target ("blah baz=blah androidboot.somethingelse somearg", &target,
                                      &error));
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
  g_test_add_func ("/prepare-root-cmdline", test_prepare_root_cmdline);
  g_test_add_func ("/prepare-root-config", test_prepare_root_config);
  return g_test_run ();
}
