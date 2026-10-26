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

#include <glib.h>

#include "ostree-fetcher.h"

static void
assert_parses_to (const char *input, const char *expected)
{
  g_autoptr (GError) error = NULL;
  OstreeFetcherURI *uri = _ostree_fetcher_uri_parse (input, &error);
  g_assert_no_error (error);
  g_assert_nonnull (uri);

  g_autofree char *str = _ostree_fetcher_uri_to_string (uri);
  g_assert_cmpstr (str, ==, expected);

  _ostree_fetcher_uri_free (uri);
}

/* Dropping a default port must keep the scheme; in particular an explicit
 * :443 must not turn an https URI into http.
 */
static void
test_fetcher_uri_default_port (void)
{
  assert_parses_to ("https://example.com:443/repo", "https://example.com/repo");
  assert_parses_to ("http://example.com:80/repo", "http://example.com/repo");
  assert_parses_to ("https://example.com:8443/repo", "https://example.com:8443/repo");
  assert_parses_to ("http://example.com:443/repo", "http://example.com:443/repo");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fetcher-uri/default-port", test_fetcher_uri_default_port);

  return g_test_run ();
}
