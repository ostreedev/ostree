/*
 * Copyright © 2018 Endless Mobile, Inc.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Authors:
 *  - Matthew Leeds <matthew.leeds@endlessm.com>
 */

#include "config.h"

#include <glib.h>
#include <locale.h>
#include <ostree.h>

static void
test_include_ostree_h_compiled (void)
{
}

/* Just ensure that we can compile with ostree.h included */
int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/include-ostree-h/compiled", test_include_ostree_h_compiled);

  return g_test_run ();
}
