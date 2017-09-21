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
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ot-keyfile-utils.h"

static GKeyFile *g_keyfile;

static void
test_get_boolean_with_default (void)
{
  g_autoptr(GError) error = NULL;
  gboolean out = FALSE;

  g_assert (ot_keyfile_get_boolean_with_default (g_keyfile,
                                                 "section",
                                                 "a_boolean_true",
                                                 FALSE,
                                                 &out,
                                                 &error));
  g_assert_true (out);

  g_assert (ot_keyfile_get_boolean_with_default (g_keyfile,
                                                 "section",
                                                 "a_boolean_false",
                                                 TRUE,
                                                 &out,
                                                 &error));
  g_assert_false (out);

  g_assert (ot_keyfile_get_boolean_with_default (g_keyfile,
                                                 "section",
                                                 "a_not_existing_boolean",
                                                 TRUE,
                                                 &out,
                                                 &error));
  g_assert_true (out);

  g_clear_error (&error);
  g_assert_false (ot_keyfile_get_boolean_with_default (g_keyfile,
                                                       "a_fake_section",
                                                       "a_boolean_true",
                                                       FALSE,
                                                       &out,
                                                       &error));
}

static void
test_get_value_with_default (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *out = NULL;
  GLogLevelFlags always_fatal_mask;
  const char *section = "section";

  /* Avoid that g_return_val_if_fail causes the test to fail.  */
  always_fatal_mask = g_log_set_always_fatal (0);


  g_assert_false (ot_keyfile_get_value_with_default (g_keyfile,
                                                     NULL,
                                                     "value_foo",
                                                     "none",
                                                     &out,
                                                     &error));
  g_clear_pointer (&out, g_free);
  g_assert_false (ot_keyfile_get_value_with_default (g_keyfile,
                                                     section,
                                                     NULL,
                                                     "none",
                                                     &out,
                                                     &error));
  g_clear_pointer (&out, g_free);
  g_assert_false (ot_keyfile_get_value_with_default (g_keyfile,
                                                     section,
                                                     NULL,
                                                     "something",
                                                     &out,
                                                     &error));
  g_clear_pointer (&out, g_free);

  /* Restore the old mask.  */
  g_log_set_always_fatal (always_fatal_mask);

  g_assert (ot_keyfile_get_value_with_default (g_keyfile,
                                               section,
                                               "value_foo",
                                               "none",
                                               &out,
                                               &error));
  g_assert_cmpstr (out, ==, "foo");
  g_clear_pointer (&out, g_free);

  g_assert (ot_keyfile_get_value_with_default (g_keyfile,
                                               section,
                                               "a_not_existing_value",
                                               "correct",
                                               &out,
                                               &error));
  g_assert_cmpstr (out, ==, "correct");
  g_clear_pointer (&out, g_free);

  g_assert_false (ot_keyfile_get_value_with_default (g_keyfile,
                                                       "a_fake_section",
                                                       "a_value_true",
                                                       "no value",
                                                       &out,
                                                       &error));
  g_clear_error (&error);
  g_clear_pointer (&out, g_free);
}

static void
test_copy_group (void)
{
  g_auto(GStrv) keys = NULL;
  g_auto(GStrv) keys2 = NULL;
  gsize length, length2, ii;
  GKeyFile *tmp = g_key_file_new ();
  const char *section = "section";
  GLogLevelFlags always_fatal_mask;

  /* Avoid that g_return_val_if_fail causes the test to fail.  */
  always_fatal_mask = g_log_set_always_fatal (0);

  g_assert_false (ot_keyfile_copy_group (NULL, tmp, section));
  g_assert_false (ot_keyfile_copy_group (g_keyfile, NULL, section));
  g_assert_false (ot_keyfile_copy_group (g_keyfile, tmp, NULL));

  /* Restore the old mask.  */
  g_log_set_always_fatal (always_fatal_mask);

  g_assert_true (ot_keyfile_copy_group (g_keyfile, tmp, section));

  keys = g_key_file_get_keys (g_keyfile, section, &length, NULL);
  keys2 = g_key_file_get_keys (tmp, section, &length2, NULL);
  g_assert_cmpint(length, ==, length2);

  for (ii = 0; ii < length; ii++)
    {
      g_autofree char *value = NULL;
      g_autofree char *value2 = NULL;

      value = g_key_file_get_value (g_keyfile, section, keys[ii], NULL);
      value2 = g_key_file_get_value (g_keyfile, section, keys[ii], NULL);
      g_assert_cmpstr (value, ==, value2);
    }

  g_key_file_free (tmp);
}

static void
fill_keyfile (GKeyFile *file)
{
  g_key_file_set_boolean (file, "section", "a_boolean_true", TRUE);
  g_key_file_set_boolean (file, "section", "a_boolean_false", FALSE);

  g_key_file_set_value (file, "section", "value_foo", "foo");
  g_key_file_set_value (file, "section", "value_bar", "bar");
}

int main (int argc, char **argv)
{
  int ret;
  g_test_init (&argc, &argv, NULL);
  g_keyfile = g_key_file_new ();
  fill_keyfile (g_keyfile);

  g_test_add_func ("/keyfile-utils/get-boolean-with-default", test_get_boolean_with_default);
  g_test_add_func ("/keyfile-utils/get-value-with-default", test_get_value_with_default);
  g_test_add_func ("/keyfile-utils/copy-group", test_copy_group);

  ret = g_test_run();

  g_key_file_free (g_keyfile);

  return ret;
}
