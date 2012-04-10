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

#include <glib/gi18n.h>
#include <glib/gprintf.h>

static gboolean quiet;
static gboolean delete;

static GOptionEntry options[] = {
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &delete, "Remove corrupted objects", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  guint n_pack_files;
} OtFsckData;

static gboolean
fsck_one_pack_file (OtFsckData        *data,
                    const char        *pack_checksum,
                    gboolean           is_meta,
                    GCancellable      *cancellable,
                    GError           **error)
{
  gboolean ret = FALSE;
  guchar objtype_u8;
  guint64 offset;
  guint64 pack_size;
  ot_lfree char *path = NULL;
  ot_lobj GFileInfo *pack_info = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *index_variant = NULL;
  ot_lobj GFile *pack_index_path = NULL;
  ot_lobj GFile *pack_data_path = NULL;
  ot_lfree guchar *pack_content_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;
  GVariantIter *index_content_iter = NULL;

  g_free (path);
  path = ostree_get_relative_pack_index_path (is_meta, pack_checksum);
  pack_index_path = g_file_resolve_relative_path (ostree_repo_get_path (data->repo), path);

  if (!ot_util_variant_map (pack_index_path,
                            OSTREE_PACK_INDEX_VARIANT_FORMAT,
                            &index_variant, error))
    goto out;
      
  if (!ostree_validate_structureof_pack_index (index_variant, error))
    goto out;

  g_free (path);
  path = ostree_get_relative_pack_data_path (is_meta, pack_checksum);
  pack_data_path = g_file_resolve_relative_path (ostree_repo_get_path (data->repo), path);
      
  input = (GInputStream*)g_file_read (pack_data_path, cancellable, error);
  if (!input)
    goto out;

  pack_info = g_file_input_stream_query_info ((GFileInputStream*)input, OSTREE_GIO_FAST_QUERYINFO,
                                              cancellable, error);
  if (!pack_info)
    goto out;
  pack_size = g_file_info_get_attribute_uint64 (pack_info, "standard::size");
     
  if (!ot_gio_checksum_stream (input, &pack_content_csum, cancellable, error))
    goto out;

  tmp_checksum = ostree_checksum_from_bytes (pack_content_csum);
  if (strcmp (tmp_checksum, pack_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "corrupted pack '%s', actual checksum is %s",
                   pack_checksum, tmp_checksum);
      goto out;
    }

  g_variant_get_child (index_variant, 2, "a(yayt)", &index_content_iter);

  while (g_variant_iter_loop (index_content_iter, "(y@ayt)",
                              &objtype_u8, NULL, &offset))
    {
      offset = GUINT64_FROM_BE (offset);
      if (offset > pack_size)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "corrupted pack '%s', offset %" G_GUINT64_FORMAT " larger than file size %" G_GUINT64_FORMAT,
                       pack_checksum,
                       offset, pack_size);
          goto out;
        }
    }

  ret = TRUE;
 out:
  if (index_content_iter)
    g_variant_iter_free (index_content_iter);
  return ret;
}

static gboolean
fsck_pack_files (OtFsckData  *data,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *meta_pack_indexes = NULL;
  ot_lptrarray GPtrArray *data_pack_indexes = NULL;
  
  if (!ostree_repo_list_pack_indexes (data->repo, &meta_pack_indexes, &data_pack_indexes,
                                      cancellable, error))
    goto out;

  for (i = 0; i < meta_pack_indexes->len; i++)
    {
      const char *pack_checksum = meta_pack_indexes->pdata[i];

      if (!fsck_one_pack_file (data, pack_checksum, TRUE, cancellable, error))
        goto out;

      data->n_pack_files++;
    }
  for (i = 0; i < data_pack_indexes->len; i++)
    {
      const char *pack_checksum = data_pack_indexes->pdata[i];

      if (!fsck_one_pack_file (data, pack_checksum, FALSE, cancellable, error))
        goto out;

      data->n_pack_files++;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fsck_reachable_objects_from_commits (OtFsckData            *data,
                                     GHashTable            *commits,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lhash GHashTable *reachable_objects = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lvariant GVariant *metadata = NULL;
  ot_lfree guchar *computed_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;

  reachable_objects = ostree_traverse_new_reachable ();

  g_hash_table_iter_init (&hash_iter, commits);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_assert (objtype == OSTREE_OBJECT_TYPE_COMMIT);

      if (!ostree_traverse_commit (data->repo, checksum, 0, reachable_objects,
                                   cancellable, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, reachable_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      OstreeObjectType checksum_objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_clear_object (&input);
      g_clear_object (&file_info);
      ot_clear_gvariant (&xattrs);

      checksum_objtype = objtype;
      
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT
          || objtype == OSTREE_OBJECT_TYPE_DIR_TREE 
          || objtype == OSTREE_OBJECT_TYPE_DIR_META)
        {
          ot_clear_gvariant (&metadata);
          if (!ostree_repo_load_variant (data->repo, objtype,
                                         checksum, &metadata, error))
            goto out;

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
      else if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
        {
          /* Handled via ARCHIVED_FILE_META */
          continue;
        }
      else if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE
               || objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META)
        {
          guint32 mode;
          if (!ostree_repo_load_file (data->repo, checksum, &input, &file_info,
                                      &xattrs, cancellable, error))
            goto out;
          checksum_objtype = OSTREE_OBJECT_TYPE_RAW_FILE; /* Override */ 

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
                                            checksum_objtype, &computed_csum,
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
  ot_lobj OstreeRepo *repo = NULL;
  ot_lhash GHashTable *objects = NULL;
  ot_lhash GHashTable *commits = NULL;

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

  ot_clear_hashtable (&objects);

  g_print ("Verifying content integrity of %u commit objects...\n",
           (guint)g_hash_table_size (commits));

  if (!fsck_reachable_objects_from_commits (&data, commits, cancellable, error))
    goto out;

  g_print ("Verifying structure of pack files...\n");

  if (!fsck_pack_files (&data, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
