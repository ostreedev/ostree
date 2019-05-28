/*
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ostree-kernel-args.h"
#include "libglnx.h"
#include "otutil.h"

#include <string.h>

struct _OstreeKernelArgs {
  GPtrArray  *order;
  GHashTable *table;
};

static char *
split_keyeq (char *arg)
{
  char *eq;

  eq = strchr (arg, '=');
  if (eq == NULL)
    return NULL;

  // Note: key/val are in a single allocation block, so we don't free val.
  *eq = '\0';
  return eq+1;
}

static gboolean
_arg_has_prefix (const char *arg,
                 char      **prefixes)
{
  char **strviter;

  for (strviter = prefixes; strviter && *strviter; strviter++)
    {
      const char *prefix = *strviter;

      if (g_str_has_prefix (arg, prefix))
        return TRUE;
    }

  return FALSE;
}

static gboolean
strcmp0_equal (gconstpointer v1,
               gconstpointer v2)
{
  return g_strcmp0 (v1, v2) == 0;
}

/**
 * ostree_kernel_args_new: (skip)
 *
 * Initializes a new OstreeKernelArgs structure and returns it
 *
 * Returns: (transfer full): A newly created #OstreeKernelArgs for kernel arguments
 *
 * Since: 2019.3
 **/
OstreeKernelArgs *
ostree_kernel_args_new (void)
{
  OstreeKernelArgs *ret;
  ret = g_new0 (OstreeKernelArgs, 1);
  ret->order = g_ptr_array_new_with_free_func (g_free);
  ret->table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      NULL, (GDestroyNotify)g_ptr_array_unref);
  return ret;
}

/**
 * ostree_kernel_args_free:
 * @kargs: An OstreeKernelArgs that represents kernel arguments
 *
 * Frees the kargs structure
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_free (OstreeKernelArgs *kargs)
{
  if (!kargs)
    return;
  g_ptr_array_unref (kargs->order);
  g_hash_table_unref (kargs->table);
  g_free (kargs);
}

/**
 * ostree_kernel_args_cleanup:
 * @loc: Address of an OstreeKernelArgs pointer
 *
 * Frees the OstreeKernelArgs structure pointed by *loc
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_cleanup (void *loc)
{
  ostree_kernel_args_free (*((OstreeKernelArgs**)loc));
}

/**
 * _ostree_kernel_arg_get_kargs_table:
 * @kargs: An OstreeKernelArgs that represents kernel arguments
 *
 * Returns: (transfer none): #GHashTable that associates with the @kargs
 *
 * Note: this function is private for now, since the data structures underneath might be changed
 *
 * Since: 2019.3
 **/
GHashTable*
_ostree_kernel_arg_get_kargs_table (OstreeKernelArgs *kargs)
{
  if (kargs != NULL)
    return kargs->table;
  return NULL;
}

/**
 * _ostree_kernel_arg_get_key_array:
 * @kargs: An OstreeKernelArgs that represents kernel arguments
 *
 * Returns: (transfer none) (element-type utf8): #GPtrArray that associates with @kargs
 *
 * Note: this function is private for now, since the data structures underneath might be changed
 *
 * Since: 2019.3
 **/
GPtrArray*
_ostree_kernel_arg_get_key_array (OstreeKernelArgs *kargs)
{
  if (kargs != NULL)
    return kargs->order;
  return NULL;
}

/**
 * ostree_kernel_args_new_replace:
 * @kargs: OstreeKernelArgs instance
 * @arg: a string argument
 * @error: error instance
 *
 * This function implements the basic logic behind key/value pair
 * replacement. Do note that the arg need to be properly formatted
 *
 * When replacing key with exact one value, the arg can be in
 * the form:
 * key, key=new_val, or key=old_val=new_val
 * The first one swaps the old_val with the key to an empty value
 * The second and third replace the old_val into the new_val
 *
 * When replacing key with multiple values, the arg can only be
 * in the form of:
 * key=old_val=new_val. Unless there is a special case where
 * there is an empty value associated with the key, then
 * key=new_val will work because old_val is empty. The empty
 * val will be swapped with the new_val in that case
 *
 * Returns: %TRUE on success, %FALSE on failure (and in some other instances such as:
 * 1. key not found in @kargs
 * 2. old value not found when @arg is in the form of key=old_val=new_val
 * 3. multiple old values found when @arg is in the form of key=old_val)
 *
 * Since: 2019.3
 **/
gboolean
ostree_kernel_args_new_replace (OstreeKernelArgs *kargs,
                                const char       *arg,
                                GError          **error)
{
  g_autofree char *arg_owned = g_strdup (arg);
  const char *key = arg_owned;
  const char *val = split_keyeq (arg_owned);

  GPtrArray *values = g_hash_table_lookup (kargs->table, key);
  if (!values)
    return glnx_throw (error, "No key '%s' found", key);
  g_assert_cmpuint (values->len, >, 0);

  /* first handle the case where the user just wants to replace an old value */
  if (val && strchr (val, '='))
    {
      g_autofree char *old_val = g_strdup (val);
      const char *new_val = split_keyeq (old_val);
      g_assert (new_val);

      guint i = 0;
      if (!ot_ptr_array_find_with_equal_func (values, old_val, strcmp0_equal, &i))
        return glnx_throw (error, "No karg '%s=%s' found", key, old_val);

      g_clear_pointer (&values->pdata[i], g_free);
      values->pdata[i] = g_strdup (new_val);
      return TRUE;
    }

  /* can't know which val to replace without the old_val=new_val syntax */
  if (values->len > 1)
    return glnx_throw (error, "Multiple values for key '%s' found", key);

  g_clear_pointer (&values->pdata[0], g_free);
  values->pdata[0] = g_strdup (val);
  return TRUE;
}

/**
 * ostree_kernel_args_delete_key_entry
 * @kargs: an OstreeKernelArgs instance
 * @key: the key to remove
 * @error: an GError instance
 *
 * This function removes the key entry from the hashtable
 * as well from the order pointer array inside kargs
 *
 * Note: since both table and order inside kernel args
 * are with free function, no extra free functions are
 * being called as they are done automatically by GLib
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 2019.3
 **/
gboolean
ostree_kernel_args_delete_key_entry (OstreeKernelArgs *kargs,
                                     const char       *key,
                                     GError          **error)
{
  if (!g_hash_table_remove (kargs->table, key))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to find kernel argument '%s'",
                   key);
      return FALSE;
    }

  /* Then remove the key from order table */
  guint key_index;
  g_assert (ot_ptr_array_find_with_equal_func (kargs->order, key, g_str_equal, &key_index));
  g_assert (g_ptr_array_remove_index (kargs->order, key_index));
  return TRUE;
}

/**
 *  ostree_kernel_args_delete:
 *  @kargs: a OstreeKernelArgs instance
 *  @arg: key or key/value pair for deletion
 *  @error: an GError instance
 *
 *  There are few scenarios being handled for deletion:
 *
 *  1: for input arg with a single key(i.e without = for split),
 *  the key/value pair will be deleted if there is only
 *  one value that is associated with the key
 *
 *  2: for input arg wth key/value pair, the specific key
 *  value pair will be deleted from the pointer array
 *  if those exist.
 *
 *  3: If the found key has only one value
 *  associated with it, the key entry in the table will also
 *  be removed, and the key will be removed from order table
 *
 *  Returns: %TRUE on success, %FALSE on failure
 *
 *  Since: 2019.3
 **/
gboolean
ostree_kernel_args_delete (OstreeKernelArgs  *kargs,
                           const char        *arg,
                           GError           **error)
{
  g_autofree char *arg_owned = g_strdup (arg);
  const char *key = arg_owned;
  const char *val = split_keyeq (arg_owned);

  GPtrArray *values = g_hash_table_lookup (kargs->table, key);
  if (!values)
    return glnx_throw (error, "No key '%s' found", key);
  g_assert_cmpuint (values->len, >, 0);

  /* special-case: we allow deleting by key only if there's only one val */
  if (values->len == 1)
    {
      /* but if a specific val was passed, check that it's the same */
      if (val && !strcmp0_equal (val, values->pdata[0]))
        return glnx_throw (error, "No karg '%s=%s' found", key, val);
      return ostree_kernel_args_delete_key_entry (kargs, key, error);
    }

  /* note val might be NULL here, in which case we're looking for `key`, not `key=` or
   * `key=val` */
  guint i = 0;
  if (!ot_ptr_array_find_with_equal_func (values, val, strcmp0_equal, &i))
    {
      if (!val)
        /* didn't find NULL -> only key= key=val1 key=val2 style things left, so the user
         * needs to be more specific */
        return glnx_throw (error, "Multiple values for key '%s' found", arg);
      return glnx_throw (error, "No karg '%s' found", arg);
    }

  g_ptr_array_remove_index (values, i);
  return TRUE;
}

/**
 * ostree_kernel_args_replace_take:
 * @kargs: a OstreeKernelArgs instance
 * @arg: (transfer full): key or key/value pair for replacement
 *
 * Finds and replaces the old key if @arg is already in the hash table,
 * otherwise adds @arg as new key and split_keyeq (arg) as value.
 * Note that when replacing old key, the old values are freed.
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_replace_take (OstreeKernelArgs   *kargs,
                                 char               *arg)
{
  gboolean existed;
  GPtrArray *values = g_ptr_array_new_with_free_func (g_free);
  const char *value = split_keyeq (arg);
  gpointer old_key;

  g_ptr_array_add (values, g_strdup (value));
  existed = g_hash_table_lookup_extended (kargs->table, arg, &old_key, NULL);

  if (existed)
    {
      g_hash_table_replace (kargs->table, old_key, values);
      g_free (arg);
    }
  else
    {
      g_ptr_array_add (kargs->order, arg);
      g_hash_table_replace (kargs->table, arg, values);
    }
}

/**
 * ostree_kernel_args_replace:
 * @kargs: a OstreeKernelArgs instance
 * @arg: key or key/value pair for replacement
 *
 * Finds and replaces the old key if @arg is already in the hash table,
 * otherwise adds @arg as new key and split_keyeq (arg) as value.
 * Note that when replacing old key value pair, the old values are freed.
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                            const char        *arg)
{
  ostree_kernel_args_replace_take (kargs, g_strdup (arg));
}

/**
 * ostree_kernel_args_append:
 * @kargs: a OstreeKernelArgs instance
 * @arg: key or key/value pair to be added
 *
 * Appends @arg which is in the form of key=value pair to the hash table kargs->table
 * (appends to the value list if key is already in the hash table)
 * and appends key to kargs->order if it is not in the hash table already.
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_append (OstreeKernelArgs  *kargs,
                           const char        *arg)
{
  gboolean existed = TRUE;
  GPtrArray *values;
  char *duped = g_strdup (arg);
  const char *val = split_keyeq (duped);

  values = g_hash_table_lookup (kargs->table, duped);
  if (!values)
    {
      values = g_ptr_array_new_with_free_func (g_free);
      existed = FALSE;
    }

  g_ptr_array_add (values, g_strdup (val));

  if (!existed)
    {
      g_hash_table_replace (kargs->table, duped, values);
      g_ptr_array_add (kargs->order, duped);
    }
  else
    {
      g_free (duped);
    }
}

/**
 * ostree_kernel_args_replace_argv:
 * @kargs: a OstreeKernelArgs instance
 * @argv: an array of key or key/value pairs
 *
 * Finds and replaces each non-null arguments of @argv in the hash table,
 * otherwise adds individual arg as new key and split_keyeq (arg) as value.
 * Note that when replacing old key value pair, the old values are freed.
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                 char             **argv)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;
      ostree_kernel_args_replace (kargs, arg);
    }
}

/**
 * ostree_kernel_args_append_argv_filtered:
 * @kargs: a OstreeKernelArgs instance
 * @argv: an array of key=value argument pairs
 * @prefixes: an array of prefix strings
 *
 * Appends each argument that does not have one of the @prefixes as prefix to the @kargs
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_append_argv_filtered (OstreeKernelArgs  *kargs,
                                         char             **argv,
                                         char             **prefixes)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;

      if (!_arg_has_prefix (arg, prefixes))
        ostree_kernel_args_append (kargs, arg);
    }
}

/**
 * ostree_kernel_args_append_argv:
 * @kargs: a OstreeKernelArgs instance
 * @argv: an array of key=value argument pairs
 *
 * Appends each value in @argv to the corresponding value array and
 * appends key to kargs->order if it is not in the hash table already.
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                char             **argv)
{
  ostree_kernel_args_append_argv_filtered (kargs, argv, NULL);
}

/**
 * ostree_kernel_args_append_proc_cmdline:
 * @kargs: a OstreeKernelArgs instance
 * @cancellable: optional GCancellable object, NULL to ignore
 * @error: an GError instance
 *
 * Appends the command line arguments in the file "/proc/cmdline"
 * that does not have "BOOT_IMAGE=" and "initrd=" as prefixes to the @kargs
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 2019.3
 **/
gboolean
ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                        GCancellable     *cancellable,
                                        GError          **error)
{
  g_autoptr(GFile) proc_cmdline_path = g_file_new_for_path ("/proc/cmdline");
  g_autofree char *proc_cmdline = NULL;
  gsize proc_cmdline_len = 0;
  g_auto(GStrv) proc_cmdline_args = NULL;
  /* When updating the filter list don't forget to update the list in the tests
   * e.g. tests/test-admin-deploy-karg.sh and
   * tests/test-admin-instutil-set-kargs.sh
   */
  char *filtered_prefixes[] = { "BOOT_IMAGE=", /* GRUB 2 */
                                "initrd=", /* sd-boot */
                                NULL };

  if (!g_file_load_contents (proc_cmdline_path, cancellable,
                             &proc_cmdline, &proc_cmdline_len,
                             NULL, error))
    return FALSE;

  g_strchomp (proc_cmdline);

  proc_cmdline_args = g_strsplit (proc_cmdline, " ", -1);
  ostree_kernel_args_append_argv_filtered (kargs, proc_cmdline_args,
                                            filtered_prefixes);

  return TRUE;
}

/**
 * ostree_kernel_args_parse_append:
 * @kargs: a OstreeKernelArgs instance
 * @options: a string representing command line arguments
 *
 * Parses @options by separating it by whitespaces and appends each argument to @kargs
 *
 * Since: 2019.3
 **/
void
ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
                                 const char       *options)
{
  char **args = NULL;
  char **iter;

  if (!options)
    return;

  args = g_strsplit (options, " ", -1);
  for (iter = args; *iter; iter++)
    {
      char *arg = *iter;
      ostree_kernel_args_append (kargs, arg);
    }
  g_strfreev (args);
}

/**
 * ostree_kernel_args_from_string: (skip)
 * @options: a string representing command line arguments
 *
 * Initializes a new OstreeKernelArgs then parses and appends @options
 * to the empty OstreeKernelArgs
 *
 * Returns: (transfer full): newly allocated #OstreeKernelArgs with @options appended
 *
 * Since: 2019.3
 **/
OstreeKernelArgs *
ostree_kernel_args_from_string (const char *options)
{
  OstreeKernelArgs *ret;

  ret = ostree_kernel_args_new ();
  ostree_kernel_args_parse_append (ret, options);

  return ret;
}

/**
 * ostree_kernel_args_to_strv:
 * @kargs: a OstreeKernelArgs instance
 *
 * Extracts all key value pairs in @kargs and appends to a temporary
 * array in forms of "key=value" or "key" if value is NULL, and returns
 * the temporary array with the GPtrArray wrapper freed
 *
 * Returns: (transfer full): an array of "key=value" pairs or "key" if value is NULL
 *
 * Since: 2019.3
 **/
char **
ostree_kernel_args_to_strv (OstreeKernelArgs *kargs)
{
  GPtrArray *strv = g_ptr_array_new ();
  guint i;

  for (i = 0; i < kargs->order->len; i++)
    {
      const char *key = kargs->order->pdata[i];
      GPtrArray *values = g_hash_table_lookup (kargs->table, key);
      guint j;

      g_assert (values != NULL);

      for (j = 0; j < values->len; j++)
        {
          const char *value = values->pdata[j];
          if (value == NULL)
            g_ptr_array_add (strv, g_strconcat (key, NULL));
          else
            g_ptr_array_add (strv, g_strconcat (key, "=", value, NULL));
        }
    }
  g_ptr_array_add (strv, NULL);

  return (char**)g_ptr_array_free (strv, FALSE);
}

/**
 * ostree_kernel_args_to_string:
 * @kargs: a OstreeKernelArgs instance
 *
 * Extracts all key value pairs in @kargs and appends to a temporary
 * GString in forms of "key=value" or "key" if value is NULL separated
 * by a single whitespace, and returns the temporary string with the
 * GString wrapper freed
 *
 * Note: the application will be terminated if one of the values array
 * in @kargs is NULL
 *
 * Returns: (transfer full): a string of "key=value" pairs or "key" if value is NULL,
 * separated by single whitespaces
 *
 * Since: 2019.3
 **/
char *
ostree_kernel_args_to_string (OstreeKernelArgs *kargs)
{
  GString *buf = g_string_new ("");
  gboolean first = TRUE;
  guint i;

  for (i = 0; i < kargs->order->len; i++)
    {
      const char *key = kargs->order->pdata[i];
      GPtrArray *values = g_hash_table_lookup (kargs->table, key);
      guint j;

      g_assert (values != NULL);

      for (j = 0; j < values->len; j++)
        {
          const char *value = values->pdata[j];

          if (first)
            first = FALSE;
          else
            g_string_append_c (buf, ' ');

          g_string_append (buf, key);
          if (value != NULL)
            {
              g_string_append_c (buf, '=');
              g_string_append (buf, value);
            }
        }
    }

  return g_string_free (buf, FALSE);
}

/**
 * ostree_kernel_args_get_last_value:
 * @kargs: a OstreeKernelArgs instance
 * @key: a key to look for in @kargs hash table
 *
 * Finds and returns the last element of value array
 * corresponding to the @key in @kargs hash table. Note that the application
 * will be terminated if the @key is found but the value array is empty
 *
 * Returns: NULL if @key is not found in the @kargs hash table,
 * otherwise returns last element of value array corresponding to @key
 *
 * Since: 2019.3
 **/
const char *
ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key)
{
  GPtrArray *values = g_hash_table_lookup (kargs->table, key);

  if (!values)
    return NULL;

  g_assert (values->len > 0);
  return (char*)values->pdata[values->len-1];
}
