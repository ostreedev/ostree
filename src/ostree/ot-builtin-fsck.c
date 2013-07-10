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

static gboolean quiet;
static gboolean delete;

static GOptionEntry options[] = {
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &delete, "Remove corrupted objects", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
} OtFsckData;

static gboolean
fsck_reachable_objects_from_commits (OtFsckData            *data,
                                     GHashTable            *commits,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gs_unref_hashtable GHashTable *reachable_objects = NULL;
  gs_unref_object GInputStream *input = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_free guchar *computed_csum = NULL;
  gs_free char *tmp_checksum = NULL;

  reachable_objects = ostree_repo_traverse_new_reachable ();

  g_hash_table_iter_init (&hash_iter, commits);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_assert (objtype == OSTREE_OBJECT_TYPE_COMMIT);

      if (!ostree_repo_traverse_commit (data->repo, checksum, 0, reachable_objects,
                                        cancellable, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, reachable_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_clear_object (&input);
      g_clear_object (&file_info);
      g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT
          || objtype == OSTREE_OBJECT_TYPE_DIR_TREE 
          || objtype == OSTREE_OBJECT_TYPE_DIR_META)
        {
          g_clear_pointer (&metadata, (GDestroyNotify) g_variant_unref);
          if (!ostree_repo_load_variant (data->repo, objtype,
                                         checksum, &metadata, error))
            {
              g_prefix_error (error, "Loading metadata object %s: ", checksum);
              goto out;
            }

          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!ostree_validate_structureof_commit (metadata, error))
                {
                  g_prefix_error (error, "While validating commit metadata '%s': ", checksum);
                  goto out;
                }
            }
          else if (objtype == OSTREE_OBJECT_TYPE_DIR_TREE)
            {
              if (!ostree_validate_structureof_dirtree (metadata, error))
                {
                  g_prefix_error (error, "While validating directory tree '%s': ", checksum);
                  goto out;
                }
            }
          else if (objtype == OSTREE_OBJECT_TYPE_DIR_META)
            {
              if (!ostree_validate_structureof_dirmeta (metadata, error))
                {
                  g_prefix_error (error, "While validating directory metadata '%s': ", checksum);
                  goto out;
                }
            }
          else
            g_assert_not_reached ();
          
          input = g_memory_input_stream_new_from_data (g_variant_get_data (metadata),
                                                       g_variant_get_size (metadata),
                                                       NULL);
        }
      else if (objtype == OSTREE_OBJECT_TYPE_FILE)
        {
          guint32 mode;
          if (!ostree_repo_load_file (data->repo, checksum, &input, &file_info,
                                      &xattrs, cancellable, error))
            {
              g_prefix_error (error, "Loading file object %s: ", checksum);
              goto out;
            }

          mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
          if (!ostree_validate_structureof_file_mode (mode, error))
            {
              g_prefix_error (error, "While validating file '%s': ", checksum);
              goto out;
            }
        }
      else
        {
          g_assert_not_reached ();
        }

      g_free (computed_csum);
      if (!ostree_checksum_file_from_input (file_info, xattrs, input,
                                            objtype, &computed_csum,
                                            cancellable, error))
        goto out;

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes (computed_csum);
      if (strcmp (checksum, tmp_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "corrupted object %s.%s; actual checksum: %s",
                       checksum, ostree_object_type_to_string (objtype),
                       tmp_checksum);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_fsck (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  OtFsckData data;
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_hashtable GHashTable *objects = NULL;
  gs_unref_hashtable GHashTable *commits = NULL;

  context = g_option_context_new ("- Check the repository for consistency");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  memset (&data, 0, sizeof (data));
  data.repo = repo;

  g_print ("Enumerating objects...\n");

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    goto out;

  commits = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                   (GDestroyNotify)g_variant_unref, NULL);
  
  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        g_hash_table_insert (commits, g_variant_ref (serialized_key), serialized_key);
    }

  g_clear_pointer (&objects, (GDestroyNotify) g_hash_table_unref);

  g_print ("Verifying content integrity of %u commit objects...\n",
           (guint)g_hash_table_size (commits));

  if (!fsck_reachable_objects_from_commits (&data, commits, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
