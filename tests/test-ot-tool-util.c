/*
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
#include "ostree-mutable-tree.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ot-tool-util.h"

/*


gboolean
ot_parse_boolean (const char  *value,
                  gboolean    *out_parsed,
                  GError     **error);
gboolean
ot_parse_keyvalue (const char  *keyvalue,
                   char       **out_key,
                   char       **out_value,
                   GError     **error);
*/
static void
test_ot_parse_boolean (void)
{
  g_autoptr(GError) error = NULL;
  gboolean out = FALSE;
  g_assert_true (ot_parse_boolean ("yes", &out, &error));
  g_assert_true (out);

  out = FALSE;
  g_assert_true (ot_parse_boolean ("1", &out, &error));
  g_assert_true (out);

  out = FALSE;
  g_assert_true (ot_parse_boolean ("true", &out, &error));
  g_assert_true (out);

  g_assert_true (ot_parse_boolean ("false", &out, &error));
  g_assert_false (out);

  out = TRUE;
  g_assert_true (ot_parse_boolean ("no", &out, &error));
  g_assert_false (out);

  out = TRUE;
  g_assert_true (ot_parse_boolean ("0", &out, &error));
  g_assert_false (out);

  out = TRUE;
  g_assert_true (ot_parse_boolean ("none", &out, &error));
  g_assert_false (out);
  g_clear_error (&error);

  g_assert_false (ot_parse_boolean ("FOO", &out, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
}

static void
test_ot_parse_keyvalue (void)
{
  g_autoptr(GError) error = NULL;
  char *keyvalue[] = {"foo=bar", "a=", "b=1231231"};
  char *key[] = {"foo", "a", "b"};
  char *value[] = {"bar", "", "1231231"};
  guint i;

  for (i = 0; i < G_N_ELEMENTS (keyvalue); i++)
    {
      g_autofree char *out_key = NULL;
      g_autofree char *out_value = NULL;
      g_assert_true (ot_parse_keyvalue (keyvalue[i],
                                        &out_key,
                                        &out_value,
                                        &error));
      g_assert_cmpstr (out_key, ==, key[i]);
      g_assert_cmpstr (out_value, ==, value[i]);
    }

  {
    g_autofree char *out_key = NULL;
    g_autofree char *out_value = NULL;
    g_assert_false (ot_parse_keyvalue ("blabla",
                                       &out_key,
                                       &out_value,
                                       &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error (&error);
  }
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ot-tool-util/parse-boolean", test_ot_parse_boolean);
  g_test_add_func ("/ot-tool-util/parse-keyvalue", test_ot_parse_keyvalue);
  return g_test_run();
}
