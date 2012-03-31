/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
} OtUnpackData;

static gboolean
gather_packed (OtUnpackData  *data,
               GHashTable    *objects,
               GHashTable   **out_packed,
               GCancellable  *cancellable,
               GError       **error)
{
  gboolean ret = FALSE;
  GHashTable *ret_packed = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  GVariant *pack_array = NULL;

  ret_packed = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                      (GDestroyNotify) g_variant_unref,
                                      NULL);

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *key_copy;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      gboolean is_packed;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      ot_clear_gvariant (&pack_array);
      g_variant_get (objdata, "(b@as)", &is_loose, &pack_array);

      is_packed = g_variant_n_children (pack_array) > 0;
      
      if (is_loose)
        continue;

      g_assert (is_packed);

      key_copy = g_variant_ref (serialized_key);
      g_hash_table_replace (ret_packed, key_copy, key_copy);
    }

  ret = TRUE;
  ot_transfer_out_value (out_packed, &ret_packed);
 /* out: */
  ot_clear_gvariant (&pack_array);
  if (ret_packed)
    g_hash_table_unref (ret_packed);
  return ret;
}

static gboolean
unpack_one_object (OstreeRepo        *repo,
                   const char        *checksum,
                   OstreeObjectType   objtype,
                   GCancellable      *cancellable,
                   GError           **error)
{
  gboolean ret = FALSE;
  GInputStream *input = NULL;
  GFileInfo *file_info = NULL;
  GVariant *xattrs = NULL;
  GVariant *meta = NULL;
  GVariant *serialized_meta = NULL;

  g_assert (objtype != OSTREE_OBJECT_TYPE_RAW_FILE);

  if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META)
    {
      if (!ostree_repo_load_file (repo, checksum,
                                  &input, &file_info, &xattrs,
                                  cancellable, error))
        goto out;

      if (!ostree_repo_stage_object_trusted (repo, OSTREE_OBJECT_TYPE_RAW_FILE,
                                             checksum, TRUE, file_info, xattrs, input,
                                             cancellable, error))
        goto out;
    }
  else if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
    {
      /* nothing; handled in META case */
    }
  else
    {
      if (!ostree_repo_load_variant (repo, objtype, checksum, &meta, error))
        goto out;

      serialized_meta = ostree_wrap_metadata_variant (objtype, meta);

      input = g_memory_input_stream_new_from_data (g_variant_get_data (serialized_meta),
                                                   g_variant_get_size (serialized_meta), NULL);
      
      if (!ostree_repo_stage_object_trusted (repo, objtype, checksum, TRUE,
                                             NULL, NULL, input, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&input);
  g_clear_object (&file_info);
  ot_clear_gvariant (&xattrs);
  ot_clear_gvariant (&meta);
  ot_clear_gvariant (&serialized_meta);
  return ret;
}

static gboolean
delete_one_packfile (OstreeRepo        *repo,
                     const char        *pack_checksum,
                     GCancellable      *cancellable,
                     GError           **error)
{
  gboolean ret = FALSE;
  GFile *data_path = NULL;
  GFile *index_path = NULL;

  index_path = ostree_repo_get_pack_index_path (repo, pack_checksum);
  data_path = ostree_repo_get_pack_data_path (repo, pack_checksum);

  if (!ot_gfile_unlink (index_path, cancellable, error))
    {
      g_prefix_error (error, "Failed to delete pack index '%s': ", ot_gfile_get_path_cached (index_path));
      goto out;
    }
  if (!ot_gfile_unlink (data_path, cancellable, error))
    {
      g_prefix_error (error, "Failed to delete pack data '%s': ", ot_gfile_get_path_cached (data_path));
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&index_path);
  g_clear_object (&data_path);
  return ret;
}

gboolean
ostree_builtin_unpack (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gboolean in_transaction = FALSE;
  OtUnpackData data;
  OstreeRepo *repo = NULL;
  GHashTable *objects = NULL;
  GCancellable *cancellable = NULL;
  GPtrArray *clusters = NULL;
  GHashTable *packed_objects = NULL;
  GHashTableIter hash_iter;
  GHashTable *packfiles_to_delete = NULL;
  gpointer key, value;
  GFile *objpath = NULL;
  guint64 unpacked_object_count = 0;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Uncompress objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (ostree_repo_get_mode (repo) != OSTREE_REPO_MODE_ARCHIVE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Can't unpack bare repositories yet");
      goto out;
    }

  data.repo = repo;

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects, cancellable, error))
    goto out;

  if (!gather_packed (&data, objects, &packed_objects, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, cancellable, error))
    goto out;

  in_transaction = TRUE;

  packfiles_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_hash_table_iter_init (&hash_iter, packed_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *objkey = key;
      GVariant *objdata;
      const char *checksum;
      const char *pack_checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      GVariantIter *pack_array_iter;
      
      objdata = g_hash_table_lookup (objects, objkey);
      g_assert (objdata);

      g_variant_get (objdata, "(bas)", &is_loose, &pack_array_iter);

      g_assert (!is_loose);

      while (g_variant_iter_loop (pack_array_iter, "&s", &pack_checksum))
        {
          if (!g_hash_table_lookup (packfiles_to_delete, pack_checksum))
            {
              gchar *duped_checksum = g_strdup (pack_checksum);
              g_hash_table_replace (packfiles_to_delete, duped_checksum, duped_checksum);
            }
        }
      g_variant_iter_free (pack_array_iter);

      ostree_object_name_deserialize (objkey, &checksum, &objtype);

      if (!unpack_one_object (repo, checksum, objtype, cancellable, error))
        goto out;

      unpacked_object_count++;
    }

  if (!ostree_repo_commit_transaction (repo, cancellable, error))
    goto out;

  if (g_hash_table_size (packfiles_to_delete) == 0)
    g_print ("No pack files; nothing to do\n");

  g_hash_table_iter_init (&hash_iter, packfiles_to_delete);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *pack_checksum = key;

      if (!delete_one_packfile (repo, pack_checksum, cancellable, error))
        goto out;
      
      g_print ("Deleted packfile '%s'\n", pack_checksum);
    }

  ret = TRUE;
 out:
  if (in_transaction)
    (void) ostree_repo_abort_transaction (repo, cancellable, NULL);
  g_clear_object (&objpath);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (clusters)
    g_ptr_array_unref (clusters);
  if (packfiles_to_delete)
    g_hash_table_unref (packfiles_to_delete);
  if (packed_objects)
    g_hash_table_unref (packed_objects);
  if (objects)
    g_hash_table_unref (objects);
  return ret;
}
