/*
 * Copyright (C) 2015 Red Hat, Inc.
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
#include "ostree-kernel-args.h"
#include "otutil.h"

static gboolean
check_string_existance (OstreeKernelArgs *karg,
                        const char *string_to_find)
{
  g_autofree gchar* string_with_spaces = ostree_kernel_args_to_string (karg);
  g_auto(GStrv) string_list = g_strsplit (string_with_spaces, " ", -1);
  return g_strv_contains ((const char* const*) string_list, string_to_find);
}

static gboolean
kernel_args_entry_value_equal (gconstpointer data,
                               gconstpointer value)
{
  const OstreeKernelArgsEntry *e = data;
  return g_strcmp0 (_ostree_kernel_args_entry_get_value (e), value) == 0;
}

static gboolean
kernel_args_entry_key_equal (gconstpointer data,
                             gconstpointer key)
{
  const OstreeKernelArgsEntry *e = data;
  return g_strcmp0 (_ostree_kernel_args_entry_get_key (e), key) == 0;
}

static void
test_kargs_delete (void)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;
  __attribute__((cleanup(ostree_kernel_args_cleanup))) OstreeKernelArgs *karg = ostree_kernel_args_new ();

  ostree_kernel_args_append (karg, "single_key=test");
  ostree_kernel_args_append (karg, "test=firstval");
  ostree_kernel_args_append (karg, "test=secondval");
  ostree_kernel_args_append (karg, "test=");
  ostree_kernel_args_append (karg, "test");

  /* Delete a non-existant key should fail */
  ret = ostree_kernel_args_delete (karg, "non_existant_key", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with multiple values when only specifying key should work if a no-value
   * variant exists */
  ret = ostree_kernel_args_delete (karg, "test", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test"));

  /* Trying again now should fail since there are only kargs with various values */
  ret = ostree_kernel_args_delete (karg, "test", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with a non existant value should fail */
  ret = ostree_kernel_args_delete (karg, "test=non_existant_value", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with only one value should fail if the value doesn't match */
  ret = ostree_kernel_args_delete (karg, "single_key=non_existent_value", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with only one value should succeed by only specifying key */
  ret = ostree_kernel_args_delete (karg, "single_key", &error);
  g_assert_no_error (error);
  g_assert (ret);
  /* verify the value array is properly updated */
  GPtrArray *kargs_array = _ostree_kernel_arg_get_key_array (karg);
  g_assert (!ot_ptr_array_find_with_equal_func (kargs_array, "single_key", kernel_args_entry_value_equal, NULL));
  g_assert (!check_string_existance (karg, "single_key"));

  /* Delete specific key/value pair */
  ret = ostree_kernel_args_delete (karg, "test=secondval", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test=secondval"));

  /* Delete key/value pair with empty string value */
  ret = ostree_kernel_args_delete (karg, "test=", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test="));

  ret = ostree_kernel_args_delete (karg, "test=firstval", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test=firstval"));

  /* Check that we can delete duplicate keys */
  ostree_kernel_args_append (karg, "test=foo");
  ostree_kernel_args_append (karg, "test=foo");
  check_string_existance (karg, "test=foo");
  ret = ostree_kernel_args_delete (karg, "test=foo", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (check_string_existance (karg, "test=foo"));
  ret = ostree_kernel_args_delete (karg, "test=foo", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test=foo"));

  /* Make sure we also gracefully do this for key-only args */
  ostree_kernel_args_append (karg, "nosmt");
  ostree_kernel_args_append (karg, "nosmt");
  check_string_existance (karg, "nosmt");
  ret = ostree_kernel_args_delete (karg, "nosmt", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (check_string_existance (karg, "nosmt"));
  ret = ostree_kernel_args_delete (karg, "nosmt", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "nosmt"));
}

static void
test_kargs_replace (void)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;
  __attribute__((cleanup(ostree_kernel_args_cleanup))) OstreeKernelArgs *karg = ostree_kernel_args_new ();

  ostree_kernel_args_append (karg, "single_key");
  ostree_kernel_args_append (karg, "test=firstval");
  ostree_kernel_args_append (karg, "test=secondval");

  /* Replace when the input key is non-existant should fail */
  ret = ostree_kernel_args_new_replace (karg, "nonexistantkey", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace non-existant value with input key=nonexistantvalue=newvalue should fail */
  ret = ostree_kernel_args_new_replace (karg, "single_key=nonexistantval=newval", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace with input key=value will fail for a key with multiple values */
  ret = ostree_kernel_args_new_replace (karg, "test=newval", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace with input key=value for a key with single value should succeed
   * Also note, we also allow ''(empty string) valid to be a value
   */
  ret = ostree_kernel_args_new_replace (karg, "single_key=newvalue", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "single_key"));
  g_assert (check_string_existance (karg, "single_key=newvalue"));

  /* Replace with input key=value=newvalue if key and value both
   * exist, the action should succeed
   */
  ret = ostree_kernel_args_new_replace (karg, "test=firstval=newval", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test=firstval"));
  g_assert (check_string_existance (karg, "test=newval"));
}

/* In this function, we want to verify that ostree_kernel_args_append
 * and ostree_kernel_args_to_string is correct. After that
 * we will use these two functions(append and tostring) in other tests: delete and replace
 */
static void
test_kargs_append (void)
{
  __attribute__((cleanup(ostree_kernel_args_cleanup))) OstreeKernelArgs *append_arg = ostree_kernel_args_new ();
  /* Some valid cases (key=value) pair */
  ostree_kernel_args_append (append_arg, "test=valid");
  ostree_kernel_args_append (append_arg, "test=secondvalid");
  ostree_kernel_args_append (append_arg, "test=");
  ostree_kernel_args_append (append_arg, "test");
  ostree_kernel_args_append (append_arg, "second_test");

  /* We loops through the kargs inside table to verify
   * the functionality of append because at this stage
   * we have yet to find the conversion kargs to string fully "functional"
   */
  GHashTable *kargs_table = _ostree_kernel_arg_get_kargs_table (append_arg);
  GLNX_HASH_TABLE_FOREACH_KV (kargs_table, const char*, key, GPtrArray*, value_array)
    {
      if (g_str_equal (key, "test"))
        {
          g_assert (ot_ptr_array_find_with_equal_func (value_array, "valid", kernel_args_entry_value_equal, NULL));
          g_assert (ot_ptr_array_find_with_equal_func (value_array, "secondvalid", kernel_args_entry_value_equal, NULL));
          g_assert (ot_ptr_array_find_with_equal_func (value_array, "", kernel_args_entry_value_equal, NULL));
          g_assert (ot_ptr_array_find_with_equal_func (value_array, NULL, kernel_args_entry_value_equal, NULL));
        }
      else
        {
          g_assert_cmpstr (key, ==, "second_test");
          g_assert (ot_ptr_array_find_with_equal_func (value_array, NULL, kernel_args_entry_value_equal, NULL));
        }
    }

  /* verify the value array is properly updated */
  GPtrArray *kargs_array = _ostree_kernel_arg_get_key_array (append_arg);
  g_assert (ot_ptr_array_find_with_equal_func (kargs_array, "test", kernel_args_entry_key_equal, NULL));
  g_assert (ot_ptr_array_find_with_equal_func (kargs_array, "second_test", kernel_args_entry_key_equal, NULL));

  /* Up till this point, we verified that the above was all correct, we then
   * check ostree_kernel_args_to_string has the right result
   */
  g_autofree gchar* kargs_str = ostree_kernel_args_to_string (append_arg);
  g_auto(GStrv) kargs_list = g_strsplit(kargs_str, " ", -1);
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test=valid"));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test=secondvalid"));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test="));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test"));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "second_test"));
  g_assert_cmpint (5, ==, g_strv_length (kargs_list));
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/kargs/kargs_append", test_kargs_append);
  g_test_add_func ("/kargs/kargs_delete", test_kargs_delete);
  g_test_add_func ("/kargs/kargs_replace", test_kargs_replace);
  return g_test_run ();
}
