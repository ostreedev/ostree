/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>
#include <string.h>

static gboolean verbose;

static GOptionEntry options[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Display informational messages", NULL },
  { NULL }
};

static gboolean
run_trigger (const char     *path,
             GCancellable   *cancellable,
             GError        **error)
{
  gboolean ret = FALSE;
  char *basename = NULL;
  GPtrArray *args = NULL;
  int estatus;

  basename = g_path_get_basename (path);

  args = g_ptr_array_new ();
  
  g_ptr_array_add (args, (char*)path);
  g_ptr_array_add (args, NULL);

  if (verbose)
    g_print ("Running trigger: %s\n", path);
  if (!g_spawn_sync (NULL,
                     (char**)args->pdata,
                     NULL,
                     0,
                     NULL, NULL, NULL, NULL,
                     &estatus,
                     error))
    {
      g_prefix_error (error, "Failed to run trigger %s: ", basename);
      goto out;
    }

  ret = TRUE;
 out:
  g_free (basename);
  if (args)
    g_ptr_array_free (args, TRUE);
  return ret;
}

static int
compare_files_by_basename (gconstpointer  ap,
                           gconstpointer  bp)
{
  GFile *a = *(GFile**)ap;
  GFile *b = *(GFile**)ap;
  char *name_a, *name_b;
  int c;

  name_a = g_file_get_basename (a);
  name_b = g_file_get_basename (b);
  c = strcmp (name_a, name_b);
  g_free (name_b);
  g_free (name_a);
  return c;
}

static gboolean
get_sorted_triggers (GPtrArray       **out_triggers,
                     GCancellable     *cancellable,
                     GError          **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  char *triggerdir_path = NULL;
  GFile *triggerdir = NULL;
  GFileInfo *file_info = NULL;
  GFileEnumerator *enumerator = NULL;
  GPtrArray *ret_triggers = NULL;

  ret_triggers = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  triggerdir_path = g_build_filename (LIBEXECDIR, "ostree", "triggers.d", NULL);
  triggerdir = g_file_new_for_path (triggerdir_path);

  enumerator = g_file_enumerate_children (triggerdir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, ".trigger"))
        {
          char *child_path;
          GFile *child;

          child_path = g_build_filename (triggerdir_path, name, NULL);
          child = g_file_new_for_path (child_path);
          g_free (child_path);

          g_ptr_array_add (ret_triggers, child);
        }
      g_clear_object (&file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  
  g_ptr_array_sort (ret_triggers, compare_files_by_basename);

  ret = TRUE;
  if (out_triggers)
    {
      *out_triggers = ret_triggers;
      ret_triggers = NULL;
    }
 out:
  g_free (triggerdir_path);
  g_clear_object (&triggerdir);
  g_clear_object (&enumerator);
  if (ret_triggers)
    g_ptr_array_unref (ret_triggers);
  return ret;
}

gboolean
run_triggers (GCancellable   *cancellable,
              GError        **error)
{
  gboolean ret = FALSE;
  int i;
  GPtrArray *triggers = NULL;
  char *path = NULL;

  if (!get_sorted_triggers (&triggers, cancellable, error))
    goto out;

  for (i = 0; i < triggers->len; i++)
    {
      const char *basename;
      GFile *trigger_path = triggers->pdata[i];

      g_free (path);
      path = g_file_get_path (trigger_path);
      basename = strrchr (path, '/');
      if (basename)
        basename += 1;
      else
        basename = path;

      g_print ("ostree-run-triggers: %s\n", basename);
      if (!run_trigger (path, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_free (path);
  if (triggers)
    g_ptr_array_unref (triggers);
  return ret;
}

int
main (int    argc,
      char **argv)
{
  GOptionContext *context;
  GError *real_error = NULL;
  GError **error = &real_error;
  GCancellable *cancellable = NULL;
  gboolean ret = FALSE;

  g_type_init ();

  context = g_option_context_new ("- Regenerate caches in operating system tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!run_triggers (cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (real_error)
    g_printerr ("%s\n", real_error->message);
  g_clear_error (&real_error);
  if (!ret)
    return 1;
  return 0;
}
