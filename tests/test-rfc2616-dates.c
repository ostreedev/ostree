/*
 * Copyright © 2020 Endless OS Foundation LLC
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
 *  - Philip Withnall <pwithnall@endlessos.org>
 */

#include "config.h"

#include <glib.h>

#include "ostree-date-utils-private.h"

static void
test_ostree_parse_rfc2616_date_time (void)
{
#if GLIB_CHECK_VERSION(2, 62, 0)
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  const struct
    {
      const char *rfc2616;
      const char *expected_iso8601;  /* (nullable) if parsing is expected to fail */
    }
  tests[] =
    {
      { "Wed, 21 Oct 2015 07:28:00 GMT", "2015-10-21T07:28:00Z" },
      { "Wed, 21 Oct 2015 07:28:00", NULL },  /* too short */
      { "Wed, 21 Oct 2015 07:28:00 CEST", NULL },  /* too long; not GMT */
      { "Cat, 21 Oct 2015 07:28:00 GMT", NULL },  /* invalid day */
      { "Wed  21 Oct 2015 07:28:00 GMT", NULL },  /* no comma */
      { "Wed,21 Oct 2015 07:28:00 GMT ", NULL },  /* missing space */
      { "Wed, xx Oct 2015 07:28:00 GMT", NULL },  /* no day-of-month */
      { "Wed, 011Oct 2015 07:28:00 GMT", NULL },  /* overlong day-of-month */
      { "Wed, 00 Oct 2015 07:28:00 GMT", NULL },  /* day-of-month underflow */
      { "Wed, 32 Oct 2015 07:28:00 GMT", NULL },  /* day-of-month overflow */
      { "Wed, 21,Oct 2015 07:28:00 GMT", NULL },  /* missing space */
      { "Wed, 21 Cat 2015 07:28:00 GMT", NULL },  /* invalid month */
      { "Wed, 21 Oct,2015 07:28:00 GMT", NULL },  /* missing space */
      { "Wed, 21 Oct xxxx 07:28:00 GMT", NULL },  /* no year */
      { "Wed, 21 Oct 0201507:28:00 GMT", NULL },  /* overlong year */
      { "Wed, 21 Oct 0000 07:28:00 GMT", NULL },  /* year underflow */
      { "Wed, 21 Oct 10000 07:28:00 GM", NULL },  /* year overflow */
      { "Wed, 21 Oct 2015,07:28:00 GMT", NULL },  /* missing space */
      { "Wed, 21 Oct 2015 07 28:00 GMT", NULL },  /* missing colon */
      { "Wed, 21 Oct 2015 007:28:00 GM", NULL },  /* overlong hour */
      { "Wed, 21 Oct 2015 xx:28:00 GMT", NULL },  /* missing hour */
      { "Wed, 21 Oct 2015 -1:28:00 GMT", NULL },  /* hour underflow */
      { "Wed, 21 Oct 2015 24:28:00 GMT", NULL },  /* hour overflow */
      { "Wed, 21 Oct 2015 07:28 00 GMT", NULL },  /* missing colon */
      { "Wed, 21 Oct 2015 07:028:00 GM", NULL },  /* overlong minute */
      { "Wed, 21 Oct 2015 07:xx:00 GMT", NULL },  /* missing minute */
      { "Wed, 21 Oct 2015 07:-1:00 GMT", NULL },  /* minute underflow */
      { "Wed, 21 Oct 2015 07:60:00 GMT", NULL },  /* minute overflow */
      { "Wed, 21 Oct 2015 07:28:00CEST", NULL },  /* missing space */
      { "Wed, 21 Oct 2015 07:28:000 GM", NULL },  /* overlong second */
      { "Wed, 21 Oct 2015 07:28:xx GMT", NULL },  /* missing second */
      { "Wed, 21 Oct 2015 07:28:-1 GMT", NULL },  /* seconds underflow */
      { "Wed, 21 Oct 2015 07:28:61 GMT", NULL },  /* seconds overflow */
      { "Wed, 21 Oct 2015 07:28:00 UTC", NULL },  /* invalid timezone (only GMT is allowed) */
      { "Thu, 01 Jan 1970 00:00:00 GMT", "1970-01-01T00:00:00Z" },  /* extreme but valid date */
      { "Mon, 31 Dec 9999 23:59:59 GMT", "9999-12-31T23:59:59Z" },  /* extreme but valid date */
    };

  for (gsize i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_test_message ("Test %" G_GSIZE_FORMAT ": %s", i, tests[i].rfc2616);

      /* Parse once with a trailing nul */
      g_autoptr(GDateTime) dt1 = _ostree_parse_rfc2616_date_time (tests[i].rfc2616, strlen (tests[i].rfc2616));
      if (tests[i].expected_iso8601 == NULL)
        g_assert_null (dt1);
      else
        {
          g_assert_nonnull (dt1);
          g_autofree char *iso8601 = g_date_time_format_iso8601 (dt1);
          g_assert_cmpstr (iso8601, ==, tests[i].expected_iso8601);
        }

      /* And parse again with no trailing nul */
      g_autofree char *rfc2616_no_nul = g_malloc (strlen (tests[i].rfc2616));
      memcpy (rfc2616_no_nul, tests[i].rfc2616, strlen (tests[i].rfc2616));
      g_autoptr(GDateTime) dt2 = _ostree_parse_rfc2616_date_time (rfc2616_no_nul, strlen (tests[i].rfc2616));
      if (tests[i].expected_iso8601 == NULL)
        g_assert_null (dt2);
      else
        {
          g_assert_nonnull (dt2);
          g_autofree char *iso8601 = g_date_time_format_iso8601 (dt2);
          g_assert_cmpstr (iso8601, ==, tests[i].expected_iso8601);
        }
    }
G_GNUC_END_IGNORE_DEPRECATIONS
#else
  /* GLib 2.62 is needed for g_date_time_format_iso8601(). */
  g_test_skip ("RFC 2616 date parsing test needs GLib ≥ 2.62.0");
#endif
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ostree_parse_rfc2616_date_time", test_ostree_parse_rfc2616_date_time);
  return g_test_run ();
}
