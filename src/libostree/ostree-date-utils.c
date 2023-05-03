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

#include <errno.h>
#include <glib.h>
#include <string.h>

#include "ostree-date-utils-private.h"

/* @buf must already be known to be long enough */
static gboolean
parse_uint (const char *buf, guint n_digits, guint min, guint max, guint *out)
{
  guint64 number;
  const char *end_ptr = NULL;
  gint saved_errno = 0;

  g_assert (out != NULL);

  if (!(n_digits == 2 || n_digits == 4))
    return FALSE;

  errno = 0;
  number = g_ascii_strtoull (buf, (gchar **)&end_ptr, 10);
  saved_errno = errno;

  if (!g_ascii_isdigit (buf[0]) || saved_errno != 0 || end_ptr == NULL || end_ptr != buf + n_digits
      || number < min || number > max)
    return FALSE;

  *out = number;
  return TRUE;
}

/* Locale-independent parsing for RFC 2616 date/times.
 *
 * Reference: https://tools.ietf.org/html/rfc2616#section-3.3.1
 *
 * Syntax:
 *    <day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT
 *
 * Note that this only accepts the full-year and GMT formats specified by
 * RFC 1123. It doesn’t accept RFC 850 or asctime formats.
 *
 * Example:
 *    Wed, 21 Oct 2015 07:28:00 GMT
 */
GDateTime *
_ostree_parse_rfc2616_date_time (const char *buf, size_t len)
{
  guint day_int, year_int, hour_int, minute_int, second_int;
  const char *day_names[] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
  };
  size_t day_name_index;
  const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  size_t month_name_index;

  if (len != 29)
    return NULL;

  const char *day_name = buf;
  const char *day = buf + 5;
  const char *month_name = day + 3;
  const char *year = month_name + 4;
  const char *hour = year + 5;
  const char *minute = hour + 3;
  const char *second = minute + 3;
  const char *tz = second + 3;

  for (day_name_index = 0; day_name_index < G_N_ELEMENTS (day_names); day_name_index++)
    {
      if (strncmp (day_names[day_name_index], day_name, 3) == 0)
        break;
    }
  if (day_name_index >= G_N_ELEMENTS (day_names))
    return NULL;
  /* don’t validate whether the day_name matches the rest of the date */
  if (*(day_name + 3) != ',' || *(day_name + 4) != ' ')
    return NULL;
  if (!parse_uint (day, 2, 1, 31, &day_int))
    return NULL;
  if (*(day + 2) != ' ')
    return NULL;
  for (month_name_index = 0; month_name_index < G_N_ELEMENTS (month_names); month_name_index++)
    {
      if (strncmp (month_names[month_name_index], month_name, 3) == 0)
        break;
    }
  if (month_name_index >= G_N_ELEMENTS (month_names))
    return NULL;
  if (*(month_name + 3) != ' ')
    return NULL;
  if (!parse_uint (year, 4, 0, 9999, &year_int))
    return NULL;
  if (*(year + 4) != ' ')
    return NULL;
  if (!parse_uint (hour, 2, 0, 23, &hour_int))
    return NULL;
  if (*(hour + 2) != ':')
    return NULL;
  if (!parse_uint (minute, 2, 0, 59, &minute_int))
    return NULL;
  if (*(minute + 2) != ':')
    return NULL;
  if (!parse_uint (second, 2, 0, 60, &second_int)) /* allow leap seconds */
    return NULL;
  if (*(second + 2) != ' ')
    return NULL;
  if (strncmp (tz, "GMT", 3) != 0)
    return NULL;

  return g_date_time_new_utc (year_int, month_name_index + 1, day_int, hour_int, minute_int,
                              second_int);
}
