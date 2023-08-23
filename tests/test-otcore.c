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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  otcore_ed25519_init ();
  g_test_add_func ("/ed25519", test_ed25519);
  return g_test_run ();
}
