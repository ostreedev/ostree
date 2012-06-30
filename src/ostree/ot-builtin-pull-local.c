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

#include "ot-builtins.h"
#include "ostree.h"

#include <unistd.h>
#include <stdlib.h>

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  OstreeRepo *src_repo;
  OstreeRepo *dest_repo;
} OtLocalCloneData;

static gboolean
import_one_object (OtLocalCloneData *data,
                   const char   *checksum,
                   OstreeObjectType objtype,
                   GCancellable  *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *content_path = NULL;
  ot_lobj GFileInfo *archive_info = NULL;
  ot_lvariant GVariant *metadata = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GInputStream *input = NULL;

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      ot_lobj GInputStream *file_object = NULL;
      guint64 length;

      if (!ostree_repo_load_file (data->src_repo, checksum,
                                  &input, &file_info, &xattrs,
                                  cancellable, error))
        goto out;

      if (!ostree_raw_file_to_content_stream (input, file_info, xattrs,
                                              &file_object, &length,
                                              cancellable, error))
        goto out;

      if (!ostree_repo_stage_file_object_trusted (data->dest_repo, checksum, FALSE,
                                                  file_object, length,
                                                  cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_load_variant (data->src_repo, objtype, checksum, &metadata,
                                     error))
        goto out;

      input = ot_variant_read (metadata);

      if (!ostree_repo_stage_object_trusted (data->dest_repo, objtype,
                                             checksum, FALSE, input,
                                             cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_pull_local (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  GOptionContext *context;
  const char *src_repo_path;
  int i;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lhash GHashTable *objects = NULL;
  ot_lobj GFile *src_f = NULL;
  ot_lobj GFile *src_repo_dir = NULL;
  ot_lobj GFile *dest_repo_dir = NULL;
  ot_lobj GFile *src_dir = NULL;
  ot_lobj GFile *dest_dir = NULL;
  ot_lhash GHashTable *refs_to_clone = NULL;
  ot_lhash GHashTable *source_objects = NULL;
  ot_lhash GHashTable *objects_to_copy = NULL;
  OtLocalCloneData data;

  context = g_option_context_new ("SRC_REPO [REFS...] -  Copy data from SRC_REPO");
  g_option_context_add_main_entries (context, options, NULL);

  memset (&data, 0, sizeof (data));

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  data.dest_repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (data.dest_repo, error))
    goto out;

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

  data.src_repo = ostree_repo_new (src_f);
  if (!ostree_repo_check (data.src_repo, error))
    goto out;

  src_repo_dir = g_object_ref (ostree_repo_get_path (data.src_repo));
  dest_repo_dir = g_object_ref (ostree_repo_get_path (data.dest_repo));

  if (argc == 2)
    {
      if (!ostree_repo_list_all_refs (data.src_repo, &refs_to_clone, cancellable, error))
        goto out;
    }
  else
    {
      refs_to_clone = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      for (i = 2; i < argc; i++)
        {
          const char *ref = argv[i];
          char *rev;
          
          if (!ostree_repo_resolve_rev (data.src_repo, ref, FALSE, &rev, error))
            goto out;
          
          /* Transfer ownership of rev */
          g_hash_table_insert (refs_to_clone, g_strdup (ref), rev);
        }
    }

  g_print ("Enumerating objects...\n");

  source_objects = ostree_traverse_new_reachable ();

  g_hash_table_iter_init (&hash_iter, refs_to_clone);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *checksum = value;

      if (!ostree_traverse_commit (data.src_repo, checksum, 0, source_objects, cancellable, error))
        goto out;
    }

  objects_to_copy = ostree_traverse_new_reachable ();
  g_hash_table_iter_init (&hash_iter, source_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      gboolean has_object;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!ostree_repo_has_object (data.dest_repo, objtype, checksum, &has_object,
                                   cancellable, error))
        goto out;
      if (!has_object)
        g_hash_table_insert (objects_to_copy, g_variant_ref (serialized_key), serialized_key);
    }

  g_print ("%u objects to copy\n", g_hash_table_size (objects_to_copy));

  if (!ostree_repo_prepare_transaction (data.dest_repo, cancellable, error))
    goto out;
  
  g_hash_table_iter_init (&hash_iter, objects_to_copy);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!import_one_object (&data, checksum, objtype, cancellable, error))
        goto out;
    }

  if (!ostree_repo_commit_transaction (data.dest_repo, NULL, error))
    goto out;

  g_print ("Writing %u refs\n", g_hash_table_size (refs_to_clone));

  g_hash_table_iter_init (&hash_iter, refs_to_clone);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *checksum = value;

      if (!ostree_repo_write_ref (data.dest_repo, NULL, name, checksum, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (data.src_repo)
    g_object_unref (data.src_repo);
  if (data.dest_repo)
    g_object_unref (data.dest_repo);
  if (context)
    g_option_context_free (context);
  return ret;
}
