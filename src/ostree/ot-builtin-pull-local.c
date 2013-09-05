/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include <unistd.h>
#include <stdlib.h>

#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static char *opt_remote;

static GOptionEntry options[] = {
  { "remote", 0, 0, G_OPTION_ARG_STRING, &opt_remote, "Add REMOTE to refspec", "REMOTE" },
  { NULL }
};

typedef struct {
  OstreeRepo *src_repo;
  OstreeRepo *dest_repo;
  GThreadPool *threadpool;
  GMainLoop *loop;
  int n_objects_to_check;
  volatile int n_objects_checked;
  volatile int n_objects_copied;
  GSConsole *console;
} OtLocalCloneData;

static gboolean
termination_condition (OtLocalCloneData  *self)
{
  return g_atomic_int_get (&self->n_objects_checked) == self->n_objects_to_check;
}

static gboolean
import_one_object (OtLocalCloneData *data,
                   const char   *checksum,
                   OstreeObjectType objtype,
                   GCancellable  *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      guint64 length;
      gs_unref_object GInputStream *file_object = NULL;

      if (!ostree_repo_load_object_stream (data->src_repo, objtype, checksum,
                                           &file_object, &length,
                                           cancellable, error))
        goto out;

      if (!ostree_repo_write_content_trusted (data->dest_repo, checksum,
                                              file_object, length,
                                              cancellable, error))
        goto out;
    }
  else
    {
      gs_unref_variant GVariant *metadata = NULL;

      if (!ostree_repo_load_variant (data->src_repo, objtype, checksum, &metadata,
                                     error))
        goto out;

      if (!ostree_repo_write_metadata_trusted (data->dest_repo, objtype, checksum, metadata,
                                               cancellable, error))
        goto out;
    }

  g_atomic_int_inc (&data->n_objects_copied);

  ret = TRUE;
 out:
  return ret;
}

static void
import_one_object_thread (gpointer   object,
                          gpointer   user_data)
{
  OtLocalCloneData *data = user_data;
  gs_unref_variant GVariant *serialized_key = object;
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *checksum;
  OstreeObjectType objtype;
  gboolean has_object;
  GCancellable *cancellable = NULL;

  ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

  if (!ostree_repo_has_object (data->dest_repo, objtype, checksum, &has_object,
                               cancellable, error))
    goto out;

  if (!has_object)
    {
      if (!import_one_object (data, checksum, objtype, cancellable, error))
        goto out;
    }
  
 out:
  g_atomic_int_add (&data->n_objects_checked, 1);
  if (termination_condition (data))
    g_main_context_wakeup (NULL);
  if (local_error != NULL)
    {
      g_printerr ("%s\n", local_error->message);
      exit (1);
    }
}

static gboolean
idle_print_status (gpointer user_data)
{
  OtLocalCloneData *data = user_data;
  gs_free char *str = NULL;

  str = g_strdup_printf ("pull: %d/%d scanned, %d objects copied",
                         g_atomic_int_get (&data->n_objects_checked),
                         data->n_objects_to_check,
                         g_atomic_int_get (&data->n_objects_copied));
  if (data->console)
    gs_console_begin_status_line (data->console, str, NULL, NULL);
  else
    g_print ("%s\n", str);

  return TRUE;
}

gboolean
ostree_builtin_pull_local (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *src_repo_path;
  int i;
  GHashTableIter hash_iter;
  gpointer key, value;
  gboolean transaction_resuming = FALSE;
  gs_unref_object GFile *src_f = NULL;
  gs_unref_object GFile *src_repo_dir = NULL;
  gs_unref_object GFile *dest_repo_dir = NULL;
  gs_unref_hashtable GHashTable *refs_to_clone = NULL;
  gs_unref_hashtable GHashTable *commits_to_clone = NULL;
  gs_unref_hashtable GHashTable *source_objects = NULL;
  OtLocalCloneData datav = { 0, };
  OtLocalCloneData *data = &datav;

  context = g_option_context_new ("SRC_REPO [REFS...] -  Copy data from SRC_REPO");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  data->dest_repo = g_object_ref (repo);

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "DESTINATION must be specified");
      goto out;
    }

  src_repo_path = argv[1];
  src_f = g_file_new_for_path (src_repo_path);

  data->src_repo = ostree_repo_new (src_f);
  if (!ostree_repo_open (data->src_repo, cancellable, error))
    goto out;

  data->threadpool = ot_thread_pool_new_nproc (import_one_object_thread, data);

  src_repo_dir = g_object_ref (ostree_repo_get_path (data->src_repo));
  dest_repo_dir = g_object_ref (ostree_repo_get_path (data->dest_repo));

  if (argc == 2)
    {
      if (!ostree_repo_list_refs (data->src_repo, NULL, &refs_to_clone,
                                  cancellable, error))
        goto out;
    }
  else
    {
      refs_to_clone = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      commits_to_clone = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
      for (i = 2; i < argc; i++)
        {
          const char *ref = argv[i];
          char *rev;
          
          if (ostree_validate_checksum_string (ref, NULL))
            {
              g_hash_table_insert (commits_to_clone, (char*)ref, (char*) ref);
            }
          else
            {
              if (!ostree_repo_resolve_rev (data->src_repo, ref, FALSE, &rev, error))
                goto out;
          
              /* Transfer ownership of rev */
              g_hash_table_insert (refs_to_clone, g_strdup (ref), rev);
            }
        }
    }

  if (!ostree_repo_prepare_transaction (data->dest_repo, &transaction_resuming,
                                        cancellable, error))
    goto out;

  g_print ("Enumerating objects...\n");

  source_objects = ostree_repo_traverse_new_reachable ();

  if (refs_to_clone)
    {
      g_hash_table_iter_init (&hash_iter, refs_to_clone);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          
          if (!ostree_repo_traverse_commit (data->src_repo, checksum, 0, source_objects,
                                            cancellable, error))
            goto out;
        }
    }

  if (commits_to_clone)
    {
      g_hash_table_iter_init (&hash_iter, commits_to_clone);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = key;

          if (!ostree_repo_traverse_commit (data->src_repo, checksum, 0, source_objects,
                                            cancellable, error))
            goto out;
        }
    }

  data->n_objects_to_check = g_hash_table_size (source_objects);
  g_hash_table_iter_init (&hash_iter, source_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      g_thread_pool_push (data->threadpool, g_variant_ref (serialized_key), NULL);
    }

  if (data->n_objects_to_check > 0)
    {
      data->console = gs_console_get ();

      if (data->console)
        gs_console_begin_status_line (data->console, "", NULL, NULL);

      g_timeout_add_seconds (1, idle_print_status, data);
      idle_print_status (data);

      while (!termination_condition (data))
        g_main_context_iteration (NULL, TRUE);

      idle_print_status (data);
      if (data->console)
        gs_console_end_status_line (data->console, NULL, NULL);
    }

  g_print ("Writing %u refs\n", g_hash_table_size (refs_to_clone));

  g_hash_table_iter_init (&hash_iter, refs_to_clone);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *checksum = value;
        
      ostree_repo_transaction_set_ref (data->dest_repo, opt_remote, name, checksum);
    }

  if (!ostree_repo_commit_transaction (data->dest_repo, NULL, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_pointer (&data->threadpool, (GDestroyNotify) g_thread_pool_free);
  if (data->src_repo)
    g_object_unref (data->src_repo);
  if (data->dest_repo)
    g_object_unref (data->dest_repo);
  if (context)
    g_option_context_free (context);
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
