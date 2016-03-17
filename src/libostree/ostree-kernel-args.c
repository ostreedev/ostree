/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
  if (eq)
    {
      /* Note key/val are in one malloc block,
       * so we don't free val...
       */
      *eq = '\0';
      return eq+1;
    }
  else
    {
      /* ...and this allows us to insert a constant
       * string.
       */
      return "";
    }
}

OstreeKernelArgs *
_ostree_kernel_args_new (void)
{
  OstreeKernelArgs *ret;
  ret = g_new0 (OstreeKernelArgs, 1);
  ret->order = g_ptr_array_new_with_free_func (g_free);
  ret->table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      NULL, (GDestroyNotify)g_ptr_array_unref);
  return ret;
}

void
_ostree_kernel_arg_autofree (OstreeKernelArgs *kargs)
{
  if (!kargs)
    return;
  g_ptr_array_unref (kargs->order);
  g_hash_table_unref (kargs->table);
  g_free (kargs);
}

void
_ostree_kernel_args_cleanup (void *loc)
{
  _ostree_kernel_arg_autofree (*((OstreeKernelArgs**)loc));
}

void
_ostree_kernel_args_replace_take (OstreeKernelArgs   *kargs,
                                  char               *arg)
{
  gboolean existed;
  GPtrArray *values = g_ptr_array_new_with_free_func (g_free);
  const char *value = split_keyeq (arg);

  existed = g_hash_table_remove (kargs->table, arg);
  if (!existed)
    g_ptr_array_add (kargs->order, arg);
  g_ptr_array_add (values, g_strdup (value));
  g_hash_table_replace (kargs->table, arg, values);
}

void
_ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                             const char        *arg)
{
  _ostree_kernel_args_replace_take (kargs, g_strdup (arg));
}

void
_ostree_kernel_args_append (OstreeKernelArgs  *kargs,
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

void
_ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                  char             **argv)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;
      _ostree_kernel_args_replace (kargs, arg);
    }
}

void
_ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                 char            **argv)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;
      _ostree_kernel_args_append (kargs, arg);
    }
}

gboolean
_ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                         GCancellable     *cancellable,
                                         GError          **error)
{
  g_autoptr(GFile) proc_cmdline_path = g_file_new_for_path ("/proc/cmdline");
  g_autofree char *proc_cmdline = NULL;
  gsize proc_cmdline_len = 0;
  g_auto(GStrv) proc_cmdline_args = NULL;

  if (!g_file_load_contents (proc_cmdline_path, cancellable,
                             &proc_cmdline, &proc_cmdline_len,
                             NULL, error))
    return FALSE;

  g_strchomp (proc_cmdline);

  proc_cmdline_args = g_strsplit (proc_cmdline, " ", -1);
  _ostree_kernel_args_append_argv (kargs, proc_cmdline_args);

  return TRUE;
}

void
_ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
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
      _ostree_kernel_args_append (kargs, arg);
    }
  g_strfreev (args);
}

OstreeKernelArgs *
_ostree_kernel_args_from_string (const char *options)
{
  OstreeKernelArgs *ret;

  ret = _ostree_kernel_args_new ();
  _ostree_kernel_args_parse_append (ret, options);

  return ret;
}

char **
_ostree_kernel_args_to_strv (OstreeKernelArgs *kargs)
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

          g_ptr_array_add (strv, g_strconcat (key, "=", value, NULL));
        }
    }
  g_ptr_array_add (strv, NULL);

  return (char**)g_ptr_array_free (strv, FALSE);
}

char *
_ostree_kernel_args_to_string (OstreeKernelArgs *kargs)
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

          if (value && *value)
            {
              g_string_append (buf, key);
              g_string_append_c (buf, '=');
              g_string_append (buf, value);
            }
          else
            g_string_append (buf, key);
        }
    }

  return g_string_free (buf, FALSE);
}

const char *
_ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key)
{
  GPtrArray *values = g_hash_table_lookup (kargs->table, key);

  if (!values)
    return NULL;

  g_assert (values->len > 0);
  return (char*)values->pdata[values->len-1];
}
