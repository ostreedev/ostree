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

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include "libglnx.h"
#include "ostree.h"

/* Test that valid UTF-8 refs are accepted */
static void
test_valid_utf8_refs (void)
{
  g_autoptr (GError) error = NULL;

  /* Valid ASCII ref */
  g_assert_true (ostree_validate_rev ("my-branch", &error));
  g_assert_no_error (error);

  /* Valid ref with numbers */
  g_assert_true (ostree_validate_rev ("branch-123", &error));
  g_assert_no_error (error);

  /* Valid ref with dots and underscores */
  g_assert_true (ostree_validate_rev ("my.branch_name", &error));
  g_assert_no_error (error);

  /* Valid ref with remote */
  g_assert_true (ostree_validate_rev ("remote:branch", &error));
  g_assert_no_error (error);

  /* Valid ref with slashes */
  g_assert_true (ostree_validate_rev ("path/to/branch", &error));
  g_assert_no_error (error);
}

/* Test that invalid UTF-8 refs are rejected */
static void
test_invalid_utf8_refs (void)
{
  g_autoptr (GError) error = NULL;

  /* Invalid UTF-8: 0xFF is never valid in UTF-8 */
  const char invalid_ff[] = { 'b', 'r', 'a', 'n', 'c', 'h', '\xff', '\0' };
  g_assert_false (ostree_validate_rev (invalid_ff, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* Invalid UTF-8: 0xFE is never valid in UTF-8 */
  const char invalid_fe[] = { 'b', 'r', 'a', 'n', 'c', 'h', '\xfe', '\0' };
  g_assert_false (ostree_validate_rev (invalid_fe, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* Invalid UTF-8: 0x80 without leading byte */
  const char invalid_80[] = { 'b', 'r', 'a', 'n', 'c', 'h', '\x80', '\0' };
  g_assert_false (ostree_validate_rev (invalid_80, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* Invalid UTF-8: truncated multi-byte sequence */
  const char invalid_trunc[] = { 'b', 'r', 'a', 'n', 'c', 'h', '\xc3', '\0' };
  g_assert_false (ostree_validate_rev (invalid_trunc, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* Invalid UTF-8: 0x333 as mentioned in issue #2959 */
  const char invalid_333[] = { 's', 'o', 'm', 'e', '-', 'b', 'r', 'a', 'n', 'c',
                               'h', '-', 'n', 'a', 'm', 'e', '-', 'w', 'i', 't',
                               'h', '-', 'i', 'n', 'v', 'a', 'l', 'i', 'd', '-',
                               'u', 't', 'f', '-', 's', 'y', 'm', 'b', 'o', 'l',
                               '-', '\xdb', '\0' };
  g_assert_false (ostree_validate_rev (invalid_333, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/validate/valid-utf8-refs", test_valid_utf8_refs);
  g_test_add_func ("/validate/invalid-utf8-refs", test_invalid_utf8_refs);
  return g_test_run ();
}
