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
  gpointer key, value;
  GHashTableIter hash_iter;
  ot_lhash GHashTable *ret_packed = NULL;
  ot_lvariant GVariant *pack_array = NULL;

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
  ot_lobj GInputStream *input = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lvariant GVariant *meta = NULL;

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

      input = g_memory_input_stream_new_from_data (g_variant_get_data (meta),
                                                   g_variant_get_size (meta), NULL);
      
      if (!ostree_repo_stage_object_trusted (repo, objtype, checksum, TRUE,
                                             NULL, NULL, input, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
delete_one_packfile (OstreeRepo        *repo,
                     const char        *pack_checksum,
                     gboolean           is_meta,
                     GCancellable      *cancellable,
                     GError           **error)
{
  gboolean ret = FALSE;
  ot_lfree char *data_name = NULL;
  ot_lobj GFile *data_path = NULL;
  ot_lfree char *index_name = NULL;
  ot_lobj GFile *index_path = NULL;

  index_name = ostree_get_relative_pack_index_path (is_meta, pack_checksum);
  index_path = g_file_resolve_relative_path (ostree_repo_get_path (repo), index_name);
  data_name = ostree_get_relative_pack_data_path (is_meta, pack_checksum);
  data_path = g_file_resolve_relative_path (ostree_repo_get_path (repo), data_name);

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
  return ret;
}

gboolean
ostree_builtin_unpack (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  GCancellable *cancellable = NULL;
  gboolean in_transaction = FALSE;
  OtUnpackData data;
  gpointer key, value;
  guint64 unpacked_object_count = 0;
  GHashTableIter hash_iter;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lhash GHashTable *objects = NULL;
  ot_lptrarray GPtrArray *clusters = NULL;
  ot_lhash GHashTable *packed_objects = NULL;
  ot_lhash GHashTable *meta_packfiles_to_delete = NULL;
  ot_lhash GHashTable *data_packfiles_to_delete = NULL;
  ot_lobj GFile *objpath = NULL;

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

  meta_packfiles_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data_packfiles_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

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
      GHashTable *target_hash;

      ostree_object_name_deserialize (objkey, &checksum, &objtype);
      
      objdata = g_hash_table_lookup (objects, objkey);
      g_assert (objdata);

      g_variant_get (objdata, "(bas)", &is_loose, &pack_array_iter);

      g_assert (!is_loose);

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        target_hash = meta_packfiles_to_delete;
      else
        target_hash = data_packfiles_to_delete;

      while (g_variant_iter_loop (pack_array_iter, "&s", &pack_checksum))
        {
          if (!g_hash_table_lookup (target_hash, pack_checksum))
            {
              gchar *duped_checksum = g_strdup (pack_checksum);
              g_hash_table_replace (target_hash, duped_checksum, duped_checksum);
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

  if (g_hash_table_size (meta_packfiles_to_delete) == 0
      && g_hash_table_size (data_packfiles_to_delete) == 0)
    g_print ("No pack files; nothing to do\n");

  g_hash_table_iter_init (&hash_iter, meta_packfiles_to_delete);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *pack_checksum = key;

      if (!delete_one_packfile (repo, pack_checksum, TRUE, cancellable, error))
        goto out;
      
      g_print ("Deleted packfile '%s'\n", pack_checksum);
    }

  g_hash_table_iter_init (&hash_iter, data_packfiles_to_delete);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *pack_checksum = key;

      if (!delete_one_packfile (repo, pack_checksum, FALSE, cancellable, error))
        goto out;
      
      g_print ("Deleted packfile '%s'\n", pack_checksum);
    }

  if (!ostree_repo_regenerate_pack_index (repo, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (in_transaction)
    (void) ostree_repo_abort_transaction (repo, cancellable, NULL);
  if (context)
    g_option_context_free (context);
  return ret;
}
