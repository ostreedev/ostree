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

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"

GHashTable *
ostree_traverse_new_reachable (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                (GDestroyNotify)g_variant_unref, NULL);
}

gboolean
ostree_traverse_dirtree (OstreeRepo      *repo,
                         const char      *dirtree_checksum,
                         GHashTable      *inout_reachable,
                         GCancellable    *cancellable,
                         GError         **error)
{
  gboolean ret = FALSE;
  int n, i;
  ot_lvariant GVariant *key;
  ot_lvariant GVariant *tree = NULL;
  ot_lvariant GVariant *files_variant = NULL;
  ot_lvariant GVariant *dirs_variant = NULL;
  ot_lvariant GVariant *csum_v = NULL;
  ot_lvariant GVariant *content_csum_v = NULL;
  ot_lvariant GVariant *metadata_csum_v = NULL;
  ot_lfree char *tmp_checksum = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, dirtree_checksum, &tree, error))
    goto out;

  key = ostree_object_name_serialize (dirtree_checksum, OSTREE_OBJECT_TYPE_DIR_TREE);
  if (!g_hash_table_lookup (inout_reachable, key))
    { 
      g_hash_table_insert (inout_reachable, key, key);
      key = NULL;

      /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
      files_variant = g_variant_get_child_value (tree, 0);
      n = g_variant_n_children (files_variant);
      for (i = 0; i < n; i++)
        {
          const char *filename;
      
          ot_clear_gvariant (&csum_v);
          g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum_v);
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (csum_v);
          key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_FILE);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }

      dirs_variant = g_variant_get_child_value (tree, 1);
      n = g_variant_n_children (dirs_variant);
      for (i = 0; i < n; i++)
        {
          const char *dirname;
      
          ot_clear_gvariant (&content_csum_v);
          ot_clear_gvariant (&metadata_csum_v);
          g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                               &dirname, &content_csum_v, &metadata_csum_v);
      
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (content_csum_v);
          if (!ostree_traverse_dirtree (repo, tmp_checksum, inout_reachable,
                                        cancellable, error))
            goto out;

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (metadata_csum_v);
          key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_traverse_commit (OstreeRepo      *repo,
                        const char      *commit_checksum,
                        int              maxdepth,
                        GHashTable      *inout_reachable,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *parent_csum_bytes = NULL;
  ot_lvariant GVariant *meta_csum_bytes = NULL;
  ot_lvariant GVariant *content_csum_bytes = NULL;
  ot_lvariant GVariant *key;
  ot_lvariant GVariant *commit = NULL;
  ot_lfree char*tmp_checksum = NULL;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit, error))
    goto out;
  
  key = ostree_object_name_serialize (commit_checksum, OSTREE_OBJECT_TYPE_COMMIT);
  g_hash_table_replace (inout_reachable, key, key);
  key = NULL;

  g_variant_get_child (commit, 7, "@ay", &meta_csum_bytes);
  g_free (tmp_checksum);
  tmp_checksum = ostree_checksum_from_bytes_v (meta_csum_bytes);
  key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META);
  g_hash_table_replace (inout_reachable, key, key);
  key = NULL;

  g_variant_get_child (commit, 6, "@ay", &content_csum_bytes);
  g_free (tmp_checksum);
  tmp_checksum = ostree_checksum_from_bytes_v (content_csum_bytes);
  if (!ostree_traverse_dirtree (repo, tmp_checksum, inout_reachable, cancellable, error))
    goto out;

  if (maxdepth == -1 || maxdepth > 0)
    {
      g_variant_get_child (commit, 1, "@ay", &parent_csum_bytes);

      if (g_variant_n_children (parent_csum_bytes) > 0)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (parent_csum_bytes);
          if (!ostree_traverse_commit (repo, tmp_checksum,
                                       maxdepth > 0 ? maxdepth - 1 : -1,
                                       inout_reachable, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

