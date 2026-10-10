/*
 * Copyright (C) 2026 Red Hat, Inc.
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
 */

#include "config.h"

#include "ostree-fetcher-backoff.h"
#include <glib.h>

static void
test_backoff_doubles_then_caps (void)
{
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 0), ==, 1000);
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 1), ==, 2000);
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 2), ==, 4000);
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 3), ==, 8000);
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 4), ==, 16000);
  /* 32000 would exceed the 30s cap */
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 5), ==, 30000);
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 6), ==, 30000);
  /* A large retry count must not overflow the shift, only saturate. */
  g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (1000, 1000), ==, 30000);
}

static void
test_backoff_zero_base_never_waits (void)
{
  for (guint n = 0; n < 8; n++)
    g_assert_cmpuint (_ostree_fetcher_retry_backoff_max_ms (0, n), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fetcher-backoff/doubles-then-caps", test_backoff_doubles_then_caps);
  g_test_add_func ("/fetcher-backoff/zero-base-never-waits", test_backoff_zero_base_never_waits);
  return g_test_run ();
}
