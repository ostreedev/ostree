/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "libglnx.h"
#include "ot-unix-utils.h"
#include <glib.h>

static void
test_ot_util_path_split_validate (void)
{
  const char *paths[] = {"foo/bar", "test", "foo/bar:", "a/b/c/d/e/f/g/h/i/l/m/n/o/p", NULL};
  int n_components[] = {2, 1, 2, 14, 0};
  int i;
  for (i = 0; paths[i]; i++)
    {
      GError *error = NULL;
      g_autoptr(GPtrArray) components = NULL;
      if (! ot_util_path_split_validate (paths[i], &components, &error))
        {
          int j;
          g_assert_cmpint (components->len, ==, n_components[i]);
          for (j = 0; j < components->len; j++)
            {
              g_assert (strcmp (components->pdata[i], ".."));
              g_assert_null (strchr (components->pdata[i], '/'));
            }
        }
    }
}

static void
test_ot_util_filename_validate (void)
{
  g_autoptr(GError) error = NULL;

  /* Check for valid inputs.  */
  g_assert (ot_util_filename_validate ("valid", &error));
  g_assert_no_error (error);
  g_assert (ot_util_filename_validate ("valid_file_name", &error));
  g_assert_no_error (error);
  g_assert (ot_util_filename_validate ("file.name", &error));
  g_assert_no_error (error);
  g_assert (ot_util_filename_validate ("foo..", &error));
  g_assert_no_error (error);
  g_assert (ot_util_filename_validate ("..bar", &error));
  g_assert_no_error (error);
  g_assert (ot_util_filename_validate ("baz:", &error));
  g_assert_no_error (error);

  /* Check for invalid inputs.  */
  g_assert_false (ot_util_filename_validate ("not/valid/file/name", &error));
  g_clear_error (&error);
  g_assert_false (ot_util_filename_validate (".", &error));
  g_clear_error (&error);
  g_assert_false (ot_util_filename_validate ("..", &error));
  g_clear_error (&error);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ot_util_path_split_validate", test_ot_util_path_split_validate);
  g_test_add_func ("/ot_util_filename_validate", test_ot_util_filename_validate);
  return g_test_run();
}
