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

static gboolean quiet;

static GOptionEntry options[] = {
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { NULL }
};

static gboolean
run_trigger (const char     *path,
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

static gboolean
check_trigger (GFile          *trigger,
               GError        **error)
{
  gboolean ret = FALSE;
  GInputStream *instream = NULL;
  GDataInputStream *datain = NULL;
  GError *temp_error = NULL;
  char *line;
  gsize len;
  char *ifexecutable_path = NULL;
  char *trigger_path = NULL;
  gboolean matched = TRUE;

  trigger_path = g_file_get_path (trigger);

  instream = (GInputStream*)g_file_read (trigger, NULL, error);
  if (!instream)
    goto out;
  datain = g_data_input_stream_new (instream);

  while ((line = g_data_input_stream_read_line (datain, &len, NULL, &temp_error)) != NULL)
    {
      if (g_str_has_prefix (line, "# IfExecutable: "))
        {
          char *executable = g_strdup (line + strlen ("# IfExecutable: "));
          g_strchomp (executable);
          g_free (ifexecutable_path);
          ifexecutable_path = g_find_program_in_path (executable);
          g_free (executable);
          if (!ifexecutable_path)
            {
              matched = FALSE;
              break;
            }
          break;
        }
      g_free (line);
    }
  if (line == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (matched)
    {
      if (!run_trigger (trigger_path, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  g_free (trigger_path);
  g_free (ifexecutable_path);
  g_clear_object (&instream);
  g_clear_object (&datain);
  return ret;
}


gboolean
run_triggers (GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  char *triggerdir_path = NULL;
  GFile *triggerdir = NULL;
  GFileInfo *file_info = NULL;
  GFileEnumerator *enumerator = NULL;

  triggerdir_path = g_build_filename (LIBEXECDIR, "ostree", "triggers.d", NULL);
  triggerdir = g_file_new_for_path (triggerdir_path);

  enumerator = g_file_enumerate_children (triggerdir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      char *child_path = NULL;
      GFile *child = NULL;
      gboolean success;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, ".trigger"))
        {
          child_path = g_build_filename (triggerdir_path, name, NULL);
          child = g_file_new_for_path (child_path);

          success = check_trigger (child, error);
        }
      else
        success = TRUE;

      g_object_unref (file_info);
      g_free (child_path);
      g_clear_object (&child);
      if (!success)
        goto out;
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_free (triggerdir_path);
  g_clear_object (&triggerdir);
  g_clear_object (&enumerator);
  return ret;
}

int
main (int    argc,
      char **argv)
{
  GOptionContext *context;
  GError *real_error = NULL;
  GError **error = &real_error;
  gboolean ret = FALSE;

  g_type_init ();

  context = g_option_context_new ("- Regenerate caches in operating system tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!run_triggers (error))
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
