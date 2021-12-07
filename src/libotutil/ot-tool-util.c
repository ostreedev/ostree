/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "otutil.h"
#include "ot-tool-util.h"

gboolean
ot_parse_boolean (const char  *value,
                  gboolean    *out_parsed,
                  GError     **error)
{
#define ARG_EQ(x, y) (g_ascii_strcasecmp(x, y) == 0)
  if (ARG_EQ(value, "1")
      || ARG_EQ(value, "true")
      || ARG_EQ(value, "yes"))
    *out_parsed = TRUE;
  else if (ARG_EQ(value, "0")
           || ARG_EQ(value, "false")
           || ARG_EQ(value, "no")
           || ARG_EQ(value, "none"))
    *out_parsed = FALSE;
  else
    {
      return glnx_throw (error, "Invalid boolean argument '%s'", value);
    }

  return TRUE;
}

gboolean
ot_parse_keyvalue (const char  *keyvalue,
                   char       **out_key,
                   char       **out_value,
                   GError     **error)
{
  const char *eq = strchr (keyvalue, '=');
  if (!eq)
    {
      return glnx_throw (error, "Missing '=' in KEY=VALUE for --set");
    }
  *out_key = g_strndup (keyvalue, eq - keyvalue);
  *out_value = g_strdup (eq + 1);
  return TRUE;
}

/**
 * Note: temporarily copied from GLib: https://github.com/GNOME/glib/blob/a419146578a42c760cff684292465b38df855f75/glib/garray.c#L1664
 * See documentation at: https://developer.gnome.org/glib/stable/glib-Pointer-Arrays.html#g-ptr-array-find-with-equal-func
 *
 * ot_ptr_array_find_with_equal_func: (skip)
 * @haystack: pointer array to be searched
 * @needle: pointer to look for
 * @equal_func: (nullable): the function to call for each element, which should
 *    return %TRUE when the desired element is found; or %NULL to use pointer
 *    equality
 * @index_: (optional) (out caller-allocates): return location for the index of
 *    the element, if found
 *
 * Checks whether @needle exists in @haystack, using the given @equal_func.
 * If the element is found, %TRUE is returned and the element’s index is
 * returned in @index_ (if non-%NULL). Otherwise, %FALSE is returned and @index_
 * is undefined. If @needle exists multiple times in @haystack, the index of
 * the first instance is returned.
 *
 * @equal_func is called with the element from the array as its first parameter,
 * and @needle as its second parameter. If @equal_func is %NULL, pointer
 * equality is used.
 *
 * Returns: %TRUE if @needle is one of the elements of @haystack
 * Since: 2.54
 */
gboolean
ot_ptr_array_find_with_equal_func (GPtrArray     *haystack,
                                   gconstpointer  needle,
                                   GEqualFunc     equal_func,
                                   guint         *index_)
{
  guint i;

  g_return_val_if_fail (haystack != NULL, FALSE);

  if (equal_func == NULL)
    equal_func = g_direct_equal;

  for (i = 0; i < haystack->len; i++)
    {
      if (equal_func (g_ptr_array_index (haystack, i), needle))
        {
          if (index_ != NULL)
            *index_ = i;
          return TRUE;
        }
    }

  return FALSE;
}
